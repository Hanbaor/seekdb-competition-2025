/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_bm25s_index.h"
#include "sql/das/ob_fts_bm25s_cache.h"
#include "sql/das/ob_fts_generic_filter.h"  // g_fts_generic_filter_ctx
#include "lib/ob_define.h"
#include "lib/utility/ob_print_utils.h"
#include "share/rc/ob_tenant_base.h"       // MTL, MTL_ID
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdarg>
#include <ctime>
#include <pthread.h>
#include <queue>

namespace oceanbase
{
using namespace common;
namespace sql
{

// ========== 持久化日志函数（性能测试时禁用）==========
void bm25s_log(const char* fmt, ...) {
  // 空函数，不输出任何日志
  (void)fmt;
}

// 静态成员定义
const int64_t ObFtsBM25SIndex::NUM_BUCKETS;

ObFtsBM25SIndex::ObFtsBM25SIndex()
  : is_inited_(false),
    is_finalized_(false),
    active_collectors_(0),
    tmp_dir_id_(-1),
    tmp_files_inited_(false),
    persist_file_opened_(false),
    bucket_directory_(nullptr),
    cache_allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "BM25SCache")),
    posting_lru_allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "BM25SLRU")),
    unique_docs_(),
    total_doc_len_(0),
    raw_entry_count_(0),
    total_docs_(0),
    avg_doc_len_(0.0),
    allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "BM25SIndex")),
    lock_(),
    posting_lru_map_(),
    posting_lru_head_(),
    posting_lru_tail_(),
    posting_lru_size_(0),
    posting_lru_capacity_(DEFAULT_POSTING_LRU_CAPACITY),
    posting_lru_inited_(false),
    use_posting_hashmap_(true),
    posting_lru_lock_(common::ObLatchIds::DEFAULT_SPIN_LOCK)
{
  // 初始化 TmpFile 相关数组
  for (int64_t i = 0; i < NUM_BUCKETS; i++) {
    bucket_fds_[i] = -1;
    bucket_write_offsets_[i] = 0;
    bucket_entry_counts_[i] = 0;
  }
  posting_lru_head_.next = &posting_lru_tail_;
  posting_lru_head_.prev = nullptr;
  posting_lru_tail_.prev = &posting_lru_head_;
  posting_lru_tail_.next = nullptr;
  // 初始化持久化相关
  memset(index_file_path_, 0, sizeof(index_file_path_));
  file_header_.reset();
  LOG_INFO("BM25S ObFtsBM25SIndex constructor called", K(NUM_BUCKETS));
}

ObFtsBM25SIndex::~ObFtsBM25SIndex()
{
  destroy();
}

ObFtsBM25SIndex &ObFtsBM25SIndex::get_instance()
{
  static ObFtsBM25SIndex instance;
  return instance;
}

int ObFtsBM25SIndex::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    // 已初始化
    LOG_DEBUG("BM25S init: already initialized");
  } else {
    SpinWLockGuard guard(lock_);
    if (!is_inited_) {
      LOG_INFO("BM25S init: starting initialization", K(NUM_BUCKETS));
      
      // 创建 HashMap
      if (OB_FAIL(unique_docs_.create(DEFAULT_BUCKET_COUNT,
                                              ObMemAttr(OB_SERVER_TENANT_ID, "BM25SDocSet")))) {
        LOG_WARN("BM25S init: fail to create unique docs set", K(ret));
      } else {
        raw_entry_count_ = 0;
        is_inited_ = true;
        LOG_INFO("BM25S init: initialization completed successfully", 
                 K(NUM_BUCKETS), K_(is_inited), K_(tmp_files_inited));
      }
    }
  }
  return ret;
}

int ObFtsBM25SIndex::ensure_inited()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = init();
  }
  return ret;
}

// ========== TmpFile 接口实现（Phase 1）==========

int ObFtsBM25SIndex::init_tmp_files()
{
  int ret = OB_SUCCESS;
  
  // 检查是否已初始化
  if (tmp_files_inited_) {
    LOG_DEBUG("BM25S init_tmp_files: already initialized");
    return OB_SUCCESS;
  }
  
  LOG_INFO("BM25S init_tmp_files: starting TmpFile initialization", K(NUM_BUCKETS));
  
  // 初始化固定数组（NUM_BUCKETS = 1024）
  for (int64_t i = 0; i < NUM_BUCKETS; ++i) {
    bucket_fds_[i] = -1;
    bucket_write_offsets_[i] = 0;
    new (&bucket_locks_[i]) ObSpinLock();
  }
  
  // 获取 TmpFileManager
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S init_tmp_files: tmp file manager is null", K(ret));
    return ret;
  }
  
  // 分配 TmpFile 目录
  if (OB_FAIL(tmp_mgr->alloc_dir(tmp_dir_id_))) {
    LOG_WARN("BM25S init_tmp_files: fail to alloc tmp dir", K(ret));
    return ret;
  }
  
  LOG_INFO("BM25S init_tmp_files: allocated tmp dir", K_(tmp_dir_id));
  
  // 打开 NUM_BUCKETS 个临时文件
  int64_t opened_count = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < NUM_BUCKETS; ++i) {
    if (OB_FAIL(tmp_mgr->open(bucket_fds_[i], tmp_dir_id_, "bm25s_bucket"))) {
      LOG_WARN("BM25S init_tmp_files: fail to open tmp file", K(ret), K(i), K_(tmp_dir_id));
      // 清理已打开的文件
      for (int64_t j = 0; j < i; ++j) {
        if (bucket_fds_[j] >= 0) {
          int tmp_ret = tmp_mgr->remove(bucket_fds_[j]);
          if (OB_SUCCESS != tmp_ret) {
            LOG_WARN("BM25S init_tmp_files: fail to remove tmp file during cleanup", 
                     K(tmp_ret), K(j), K(bucket_fds_[j]));
          }
          bucket_fds_[j] = -1;
        }
      }
      break;
    }
    opened_count++;
    
    // 每打开 128 个文件记录一次日志
    if ((i + 1) % 128 == 0) {
      LOG_INFO("BM25S init_tmp_files: progress", K(i), K(NUM_BUCKETS), K(opened_count));
    }
  }
  
  if (OB_SUCC(ret)) {
    tmp_files_inited_ = true;
    LOG_INFO("BM25S init_tmp_files: TmpFile initialization completed", 
             K_(tmp_dir_id), K(NUM_BUCKETS), K(opened_count));
  }
  
  return ret;
}

void ObFtsBM25SIndex::cleanup_tmp_files()
{
  int ret = OB_SUCCESS;  // LOG_WARN 需要 ret 变量
  LOG_INFO("BM25S cleanup_tmp_files: starting cleanup", K_(tmp_files_inited), K_(tmp_dir_id));
  
  if (!tmp_files_inited_) {
    LOG_DEBUG("BM25S cleanup_tmp_files: tmp files not initialized, skip");
    return;
  }
  
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S cleanup_tmp_files: tmp file manager is null during cleanup", K(ret));
    return;
  }
  
  // 删除所有桶文件（固定 NUM_BUCKETS 个）
  int64_t removed_count = 0;
  for (int64_t i = 0; i < NUM_BUCKETS; ++i) {
    if (bucket_fds_[i] >= 0) {
      int tmp_ret = tmp_mgr->remove(bucket_fds_[i]);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("BM25S cleanup_tmp_files: fail to remove tmp file", 
                 K(tmp_ret), K(i), K(bucket_fds_[i]));
      } else {
        removed_count++;
      }
      bucket_fds_[i] = -1;
      bucket_write_offsets_[i] = 0;
    }
  }
  
  tmp_files_inited_ = false;
  tmp_dir_id_ = -1;
  
  LOG_INFO("BM25S cleanup_tmp_files: cleanup completed", K(removed_count), K(NUM_BUCKETS));
}

// ========== V2: 变长记录读写函数 ==========

int ObFtsBM25SIndex::write_variable_record(int64_t bucket_id, const common::ObString &token,
                                            int64_t doc_id, int32_t tf, int32_t doc_len)
{
  int ret = OB_SUCCESS;
  
  // 参数检查
  if (bucket_id < 0 || bucket_id >= NUM_BUCKETS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S write_variable_record: invalid bucket id", K(ret), K(bucket_id), K(NUM_BUCKETS));
    return ret;
  }
  
  if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S write_variable_record: tmp file not initialized", 
             K(ret), K(bucket_id), K_(tmp_files_inited));
    return ret;
  }
  
  // 计算记录大小: [token_len:2][token_str:N][doc_id:8][tf:4][doc_len:4]
  uint16_t token_len = static_cast<uint16_t>(token.length());
  int64_t record_size = 2 + token_len + 8 + 4 + 4;
  
  // 使用栈上缓冲区，避免 ArenaAllocator 的内存累积
  char stack_buf[512]; 
  char *buf = nullptr;
  bool use_stack = (record_size <= static_cast<int64_t>(sizeof(stack_buf)));
  
  if (use_stack) {
    buf = stack_buf;
  } else {
    buf = static_cast<char*>(allocator_.alloc(record_size));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("BM25S write_variable_record: fail to alloc buffer", K(ret), K(record_size));
      return ret;
    }
  }
  
  char *p = buf;
  memcpy(p, &token_len, 2); p += 2;
  memcpy(p, token.ptr(), token_len); p += token_len;
  memcpy(p, &doc_id, 8); p += 8;
  memcpy(p, &tf, 4); p += 4;
  memcpy(p, &doc_len, 4); p += 4;
  
  // 构建 IO 信息
  tmp_file::ObTmpFileIOInfo io_info;
  io_info.fd_ = bucket_fds_[bucket_id];
  io_info.dir_id_ = tmp_dir_id_;
  io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
  io_info.buf_ = buf;
  io_info.size_ = record_size;
  io_info.io_timeout_ms_ = 5000;
  
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S write_variable_record: tmp file manager is null", K(ret));
    // 只有使用 allocator 分配的才需要 free（虽然是空操作）
    if (!use_stack) {
      allocator_.free(buf);
    }
    return ret;
  }
  
  if (OB_FAIL(tmp_mgr->write(MTL_ID(), io_info))) {
    LOG_WARN("BM25S write_variable_record: fail to write to tmp file", 
             K(ret), K(bucket_id), K(record_size));
  } else {
    ATOMIC_AAF(&bucket_write_offsets_[bucket_id], record_size);
  }
  
  // 栈上缓冲区无需释放，allocator 分配的 free 是空操作
  if (!use_stack) {
    allocator_.free(buf);
  }
  return ret;
}

int ObFtsBM25SIndex::read_variable_bucket(int64_t bucket_id, common::ObIArray<TmpRecord> &records, char *&out_buf)
{
  int ret = OB_SUCCESS;
  out_buf = nullptr;
  
  if (bucket_id < 0 || bucket_id >= NUM_BUCKETS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S read_variable_bucket: invalid bucket id", K(ret), K(bucket_id), K(NUM_BUCKETS));
    return ret;
  }
  
  if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S read_variable_bucket: tmp file not initialized", 
             K(ret), K(bucket_id), K_(tmp_files_inited));
    return ret;
  }
  
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S read_variable_bucket: tmp file manager is null", K(ret));
    return ret;
  }
  
  int64_t file_size = 0;
  if (OB_FAIL(tmp_mgr->get_tmp_file_size(bucket_fds_[bucket_id], file_size))) {
    LOG_WARN("BM25S read_variable_bucket: fail to get tmp file size", 
             K(ret), K(bucket_id), K(bucket_fds_[bucket_id]));
    return ret;
  }
  
  if (file_size == 0) {
    LOG_DEBUG("BM25S read_variable_bucket: empty bucket", K(bucket_id));
    return OB_SUCCESS;
  }
  
  LOG_DEBUG("BM25S read_variable_bucket: reading bucket", K(bucket_id), K(file_size));
  
  // 2. 分配读取缓冲区（使用 ob_malloc，支持真正释放）
  char *buf = static_cast<char*>(ob_malloc(file_size, "BM25SBucket"));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S read_variable_bucket: fail to alloc buffer", K(ret), K(file_size));
    return ret;
  }
  
  // 3. 读取整个文件
  tmp_file::ObTmpFileIOInfo io_info;
  io_info.fd_ = bucket_fds_[bucket_id];
  io_info.dir_id_ = tmp_dir_id_;
  io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
  io_info.buf_ = buf;
  io_info.size_ = file_size;
  io_info.io_timeout_ms_ = 30000;
  
  tmp_file::ObTmpFileIOHandle handle;
  if (OB_FAIL(tmp_mgr->pread(MTL_ID(), io_info, 0 /*offset*/, handle))) {
    LOG_WARN("BM25S read_variable_bucket: fail to read tmp file", 
             K(ret), K(bucket_id), K(file_size));
    ob_free(buf);
    return ret;
  }
  
  // 4. 解析变长记录（零拷贝：token 直接指向 buf）
  char *p = buf;
  char *end = buf + handle.get_done_size();
  int64_t record_count = 0;
  
  while (p < end) {
    // 读取 token_len (2 bytes)
    if (p + 2 > end) break;
    uint16_t token_len = 0;
    memcpy(&token_len, p, 2); p += 2;
    
    // 读取 token_str (token_len bytes) - 零拷贝，直接指向 buf
    if (p + token_len > end) break;
    char *token_ptr = p;  // 直接指向 buf 中的位置
    p += token_len;
    
    // 读取 doc_id, tf, doc_len (8 + 4 + 4 = 16 bytes)
    if (p + 16 > end) break;
    
    TmpRecord r;
    r.token.assign_ptr(token_ptr, token_len);  // 零拷贝
    memcpy(&r.doc_id, p, 8); p += 8;
    memcpy(&r.tf, p, 4); p += 4;
    memcpy(&r.doc_len, p, 4); p += 4;
    
    if (OB_FAIL(records.push_back(r))) {
      LOG_WARN("BM25S read_variable_bucket: fail to push record", K(ret));
      break;
    }
    record_count++;
  }
  
  // 如果解析失败，释放 buf 并清空 records
  if (OB_FAIL(ret)) {
    ob_free(buf);
    records.reset();
  } else {
    // 成功时，buf 由调用方负责释放（通过 out_buf）
    out_buf = buf;
  }
  
  LOG_INFO("BM25S read_variable_bucket: bucket read complete", K(bucket_id), K(record_count));
  return ret;
}

int ObFtsBM25SIndex::collect_raw_data(const common::ObString &token, int64_t doc_id, int32_t tf, int32_t doc_len)
{
  int ret = OB_SUCCESS;
  
  if (OB_FAIL(ensure_inited())) {
    LOG_WARN("BM25S collect_raw_data: ensure_inited failed", K(ret));
  } else if (token.empty()) {
    // 空 token，跳过
    LOG_DEBUG("BM25S collect_raw_data: empty token, skip");
  } else if (is_finalized_) {
    // 已经 finalize，跳过（无需加锁检查，因为 finalize 后不会再变）
    LOG_DEBUG("BM25S collect_raw_data: already finalized, skip");
  } else {
    // ========== 第一步：确保 TmpFile 已初始化（双重检查锁定）==========
    if (!tmp_files_inited_) {
      SpinWLockGuard guard(lock_);
      if (!tmp_files_inited_) {
        LOG_INFO("BM25S collect_raw_data: initializing tmp files");
        if (OB_FAIL(init_tmp_files())) {
          LOG_WARN("BM25S collect_raw_data: fail to init tmp files", K(ret));
          return ret;
        }
      }
    }
    
    // ========== V2: 使用 hash(token) 分桶，不需要 HashMap ==========
    // 计算桶 ID：与持久化和查询时保持一致
    uint64_t token_hash = token.hash();
    int64_t bucket_id = static_cast<int64_t>(token_hash % NUM_BUCKETS);
    
    // ========== 第二步：写入 TmpFile（桶锁保护 + 无限重试机制）==========
    // V2: 使用变长记录，直接存储 token 字符串
    // 重试机制：OB_ALLOCATE_TMP_FILE_PAGE_FAILED 是临时性错误，等待 WBP 刷盘后重试
    {
      common::ObSpinLockGuard bucket_guard(bucket_locks_[bucket_id]);
      const int MAX_RETRY = 1000;  // 设置上限避免死循环，但基本等于无限重试
      int retry_count = 0;
      do {
        ret = write_variable_record(bucket_id, token, doc_id, tf, doc_len);
        if (OB_FAIL(ret)) {
          if (ret == OB_ALLOCATE_TMP_FILE_PAGE_FAILED && retry_count < MAX_RETRY) {
            // 等待时间：min(10ms, 1ms * (retry+1))，最多等 10ms
            int wait_ms = (retry_count + 1) > 10 ? 10 : (retry_count + 1);
            usleep(wait_ms * 1000);
            retry_count++;
            ret = OB_SUCCESS;  // 清除错误码以继续重试
          } else {
            LOG_WARN("BM25S collect_raw_data: fail to write variable record after many retries", 
                     K(ret), K(bucket_id), K(token), K(doc_id), K(retry_count));
            return ret;
          }
        } else {
          break;  // 写入成功
        }
      } while (retry_count <= MAX_RETRY);
    }
    
    // 原子更新计数
    ATOMIC_AAF(&bucket_entry_counts_[bucket_id], 1);
    ATOMIC_AAF(&raw_entry_count_, 1);
    
    // 每写入 10 万条记录打印一次日志
    int64_t current_count = ATOMIC_LOAD(&raw_entry_count_);
    if (current_count % 100000 == 0) {
      LOG_INFO("BM25S collect_raw_data: V2 progress", 
               K(current_count), K(bucket_id));
    }
    
    // ========== 第四步：统计唯一文档（DocSet 读写锁）==========
    if (OB_SUCC(ret)) {
      bool doc_exists = false;
      
      // 先用读锁检查
      {
        SpinRLockGuard rguard(doc_set_lock_);
        doc_exists = (OB_HASH_EXIST == unique_docs_.exist_refactored(doc_id));
      }
      
      // 如果不存在，用写锁插入
      if (!doc_exists) {
        SpinWLockGuard wguard(doc_set_lock_);
        // 双重检查
        if (OB_HASH_NOT_EXIST == unique_docs_.exist_refactored(doc_id)) {
          if (OB_FAIL(unique_docs_.set_refactored(doc_id, 0))) {
            // set_refactored 失败，忽略（可能其他线程已插入）
            ret = OB_SUCCESS;
          } else {
            ATOMIC_AAF(&total_doc_len_, doc_len);
            LOG_DEBUG("BM25S collect_raw_data: new document registered", K(doc_id), K(doc_len));
          }
        }
      }
    }
  }
  
  return ret;
}

void ObFtsBM25SIndex::reset()
{
  LOG_INFO("BM25S reset: starting reset", K_(is_inited), K_(tmp_files_inited));
  
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    // 清理 TmpFile
    cleanup_tmp_files();
    
    // 重置桶计数
    for (int64_t i = 0; i < NUM_BUCKETS; i++) {
      bucket_entry_counts_[i] = 0;
    }
    
    unique_docs_.reuse();
    total_doc_len_ = 0;
    raw_entry_count_ = 0;
    
    total_docs_ = 0;
    avg_doc_len_ = 0.0;
    is_finalized_ = false;
    
    allocator_.reset();
    {
      common::SpinWLockGuard lru_guard(posting_lru_lock_);
      posting_lru_map_.destroy();
      posting_lru_allocator_.reset();
      posting_lru_head_.next = &posting_lru_tail_;
      posting_lru_head_.prev = nullptr;
      posting_lru_tail_.prev = &posting_lru_head_;
      posting_lru_tail_.next = nullptr;
      posting_lru_size_ = 0;
      posting_lru_inited_ = false;
    }
    LOG_INFO("BM25S reset: reset completed");
  }
}

void ObFtsBM25SIndex::destroy()
{
  LOG_INFO("BM25S destroy: starting destroy", K_(is_inited), K_(tmp_files_inited));
  
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    // 清理 TmpFile
    cleanup_tmp_files();
    
    unique_docs_.destroy();
    
    allocator_.reset();
    is_inited_ = false;
    is_finalized_ = false;
    LOG_INFO("BM25S destroy: destroy completed");
  }

  {
    common::SpinWLockGuard lru_guard(posting_lru_lock_);
    posting_lru_map_.destroy();
    posting_lru_allocator_.reset();
    posting_lru_head_.next = &posting_lru_tail_;
    posting_lru_head_.prev = nullptr;
    posting_lru_tail_.prev = &posting_lru_head_;
    posting_lru_tail_.next = nullptr;
    posting_lru_size_ = 0;
    posting_lru_inited_ = false;
  }
}

// collect_raw_data 已被 collect_document 模板替代（文件流式）

double ObFtsBM25SIndex::calc_bm25_score(int32_t tf, int32_t doc_len, int64_t df)
{
  // BM25 公式（与 ObExprBM25 完全一致）
  // 
  // query_token_weight (IDF):
  //   diff = max(0, N - df)
  //   idf = log((diff + 0.5) / (df + 0.5))
  //   query_weight = max(epsilon, idf) * (1 + k1)
  //
  // doc_token_weight (TF):
  //   norm_len = doc_len / avg_doc_len
  //   doc_weight = tf / (tf + k1 * (1 - b + b * norm_len))
  //
  // score = query_weight * doc_weight
  
  // IDF 部分
  const double diff = (total_docs_ - df) > 0 ? (total_docs_ - df) : 0;
  const double idf = std::log((diff + 0.5) / (df + 0.5));
  const double query_weight = std::max(EPSILON, idf) * (1.0 + K1);
  
  // TF 部分
  const double norm_len = doc_len / avg_doc_len_;
  const double doc_weight = static_cast<double>(tf) / 
                            (tf + K1 * (1.0 - B + B * norm_len));
  
  return query_weight * doc_weight;
}

void ObFtsBM25SIndex::print_statistics() const
{
  SpinRLockGuard guard(lock_);
  
  LOG_INFO("ObFtsBM25SIndex statistics",
           K_(is_inited), K_(is_finalized),
           K_(total_docs), K_(avg_doc_len),
           "vocab_size", file_header_.vocab_size,
           "total_entries", file_header_.total_postings,
           K_(raw_entry_count));
}

// ========== Phase 2: 持久化实现 ==========

int ObFtsBM25SIndex::get_index_path(uint64_t tenant_id, uint64_t table_id,
                                     char *path_buf, int64_t buf_len)
{
  int ret = OB_SUCCESS;
  
  // 使用固定路径 /data/obcluster/store/bm25s_index
  const char *data_dir = "/data/obcluster/store";
  
  int n = snprintf(path_buf, buf_len, "%s/bm25s_index/%lu/%lu",
                   data_dir, tenant_id, table_id);
  if (n < 0 || n >= buf_len) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("BM25S get_index_path: path too long", K(ret), K(tenant_id), K(table_id));
  }
  
  return ret;
}

float ObFtsBM25SIndex::calc_bm25_score_f(int32_t tf, int32_t doc_len, int64_t df)
{
  // BM25 参数
  const float k1 = 1.2f;
  const float b = 0.75f;
  
  // IDF = log((N - df + 0.5) / (df + 0.5) + 1)
  float idf = logf((total_docs_ - df + 0.5f) / (df + 0.5f) + 1.0f);
  
  // TF 部分 = tf * (k1 + 1) / (tf + k1 * (1 - b + b * doc_len / avg_doc_len))
  float tf_part = static_cast<float>(tf) * (k1 + 1.0f) / 
                  (tf + k1 * (1.0f - b + b * doc_len / static_cast<float>(avg_doc_len_)));
  
  return idf * tf_part;
}

int ObFtsBM25SIndex::write_header_and_directory(const common::ObIOFd &fd)
{
  int ret = OB_SUCCESS;
  int64_t write_size = 0;
  
  // 注意：不调用 file_header_.reset()，因为 caller 已经设置好了 file_header_
  // 这里只负责写入 header 和空的 bucket directory 占位符
  
  // 1. 写入 Header
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, &file_header_, 
                                                  sizeof(file_header_), 0, write_size))) {
    LOG_WARN("BM25S write_header_and_directory: fail to write header", K(ret));
    return ret;
  }
  
  // 3. 准备 Bucket Directory（先写入空的，后面更新）
  int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  char *dir_buf = static_cast<char*>(ob_malloc(dir_size, "BM25SDirBuf"));
  if (OB_ISNULL(dir_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S write_header_and_directory: fail to alloc dir_buf", K(ret), K(dir_size));
    return ret;
  }
  memset(dir_buf, 0, dir_size);
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, dir_buf, dir_size,
                                                  file_header_.bucket_dir_offset, write_size))) {
    LOG_WARN("BM25S write_header_and_directory: fail to write bucket directory", K(ret));
    ob_free(dir_buf);
    return ret;
  }
  
  ob_free(dir_buf);
  
  LOG_INFO("BM25S write_header_and_directory: completed", 
           K(file_header_.bucket_dir_offset), K(dir_size));
  
  return ret;
}

int ObFtsBM25SIndex::update_header(const common::ObIOFd &fd)
{
  int ret = OB_SUCCESS;
  int64_t write_size = 0;
  
  // 1. 更新并写入 Header
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, &file_header_,
                                                  sizeof(file_header_), 0, write_size))) {
    LOG_WARN("BM25S update_header: fail to update header", K(ret));
    return ret;
  }
  
  // 2. 更新并写入 Bucket Directory
  if (OB_NOT_NULL(bucket_directory_)) {
    int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, bucket_directory_,
                                                    dir_size, file_header_.bucket_dir_offset,
                                                    write_size))) {
      LOG_WARN("BM25S update_header: fail to update bucket directory", K(ret));
      return ret;
    }
  }
  
  LOG_INFO("BM25S update_header: completed", K(file_header_));
  
  return ret;
}

int ObFtsBM25SIndex::finalize_and_persist(const char *index_path)
{
  int ret = OB_SUCCESS;
  
  if (OB_ISNULL(index_path)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S finalize_and_persist: index_path is null", K(ret));
    return ret;
  }
  
  LOG_INFO("BM25S finalize_and_persist V2 OPT: starting", K(index_path));
  int64_t start_time = ObTimeUtility::current_time();
  
  // 0. 更新统计信息
  total_docs_ = unique_docs_.size();
  avg_doc_len_ = (total_docs_ > 0) ? 
                 static_cast<double>(total_doc_len_) / total_docs_ : 1.0;
  
  LOG_INFO("BM25S finalize_and_persist V2 OPT: statistics", 
           K_(total_docs), K_(avg_doc_len), K_(raw_entry_count));
  
  // 1. 创建持久化文件
  common::ObIOFd fd;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(index_path, 
                                          O_CREAT | O_WRONLY | O_TRUNC, 
                                          0644, fd))) {
    LOG_WARN("BM25S finalize_and_persist: fail to open file", K(ret), K(index_path));
    return ret;
  }
  
  // ========== V2 优化：逐桶统计，大幅减少内存占用 ==========
  // 每个桶使用小 HashMap，内存从 O(vocab_size) 降到 O(单桶 token 数)
  
  // 分配 Bucket Directory
  bucket_directory_ = static_cast<BucketDirectoryEntry*>(
      allocator_.alloc(NUM_BUCKETS * sizeof(BucketDirectoryEntry)));
  if (OB_ISNULL(bucket_directory_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S finalize_and_persist V2: fail to alloc bucket_directory", K(ret));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  for (int32_t i = 0; i < NUM_BUCKETS; ++i) {
    bucket_directory_[i].reset();
  }
  
  // 2. 第一遍：逐桶统计 vocab_size 和 posting 数量（计算文件布局）
  LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 1 - per-bucket stats");
  
  ObArray<int64_t> bucket_vocab_sizes;      // 每个桶的唯一 token 数
  ObArray<int64_t> bucket_posting_counts;   // 每个桶的 posting 总数
  ObArray<int64_t> bucket_vocab_bytes;      // 每个桶的 Vocab Bucket 字节数
  
  if (OB_FAIL(bucket_vocab_sizes.prepare_allocate(NUM_BUCKETS)) ||
      OB_FAIL(bucket_posting_counts.prepare_allocate(NUM_BUCKETS)) ||
      OB_FAIL(bucket_vocab_bytes.prepare_allocate(NUM_BUCKETS))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S finalize_and_persist V2: fail to alloc bucket stats", K(ret));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  int64_t total_vocab_size = 0;
  int64_t total_postings = 0;
  int64_t total_vocab_bytes = 0;
  
  typedef common::hash::ObHashMap<common::ObString, int64_t, common::hash::NoPthreadDefendMode> TokenDfMap;
  
  for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
    ObArray<TmpRecord> records;
    char *bucket_buf = nullptr;  // 零拷贝：存储桶数据的缓冲区
    
    if (OB_FAIL(read_variable_bucket(bucket_id, records, bucket_buf))) {
      LOG_WARN("BM25S finalize_and_persist V2: fail to read bucket", K(ret), K(bucket_id));
      break;  // read_variable_bucket 失败时会自己清理
    }
    
    if (records.empty()) {
      bucket_vocab_sizes.at(bucket_id) = 0;
      bucket_posting_counts.at(bucket_id) = 0;
      bucket_vocab_bytes.at(bucket_id) = 0;
      continue;
    }
    
    // 桶内小 HashMap 统计 df
    TokenDfMap bucket_df_map;
    if (OB_FAIL(bucket_df_map.create(1024, "BM25SBucketDf"))) {
      LOG_WARN("BM25S finalize_and_persist V2: fail to create bucket_df_map", K(ret), K(bucket_id));
      if (bucket_buf != nullptr) ob_free(bucket_buf);
      break;
    }
    
    int64_t bucket_postings = 0;
    for (int64_t i = 0; i < records.count(); ++i) {
      const TmpRecord &rec = records.at(i);
      int64_t df = 0;
      int tmp_ret = bucket_df_map.get_refactored(rec.token, df);
      if (OB_HASH_NOT_EXIST == tmp_ret) {
        bucket_df_map.set_refactored(rec.token, 1, 0);
      } else if (OB_SUCCESS == tmp_ret) {
        bucket_df_map.set_refactored(rec.token, df + 1, 1);
      }
      bucket_postings++;
    }
    
    // 计算桶内 vocab 字节数
    // 格式: [hash:8][len:2][token:N][offset:8][count:4][min_doc_id:4][max_doc_id:4] = 30 + N bytes
    int64_t bucket_vocab_size = bucket_df_map.size();
    int64_t vocab_bytes = 0;
    for (TokenDfMap::iterator it = bucket_df_map.begin(); it != bucket_df_map.end(); ++it) {
      vocab_bytes += 30 + it->first.length();  // 30 + N (包含 min_doc_id 和 max_doc_id)
    }
    
    bucket_vocab_sizes.at(bucket_id) = bucket_vocab_size;
    bucket_posting_counts.at(bucket_id) = bucket_postings;
    bucket_vocab_bytes.at(bucket_id) = vocab_bytes;
    
    total_vocab_size += bucket_vocab_size;
    total_postings += bucket_postings;
    total_vocab_bytes += vocab_bytes;
    
    bucket_df_map.destroy();
    
    // 零拷贝优化：只需释放一个 bucket_buf，不需要逐个释放 token
    if (bucket_buf != nullptr) {
      ob_free(bucket_buf);
    }
    
    if ((bucket_id + 1) % 256 == 0) {
      LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 1 progress", 
               K(bucket_id), K(NUM_BUCKETS), K(total_vocab_size), K(total_postings));
    }
  }
  
  if (OB_FAIL(ret)) {
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 1 completed", 
           K(total_vocab_size), K(total_postings), K(total_vocab_bytes));
  
  // 3. 计算文件布局
  int64_t posting_data_size = total_postings * 8;  // 每个 posting = 8 bytes
  
  file_header_.reset();
  file_header_.magic = BM25SIndexFileHeader::MAGIC;
  file_header_.version = BM25SIndexFileHeader::CURRENT_VERSION;
  file_header_.total_docs = total_docs_;
  file_header_.avg_doc_len = avg_doc_len_;
  file_header_.vocab_size = total_vocab_size;
  file_header_.total_postings = total_postings;
  file_header_.bucket_num = NUM_BUCKETS;
  file_header_.create_timestamp = ObTimeUtility::current_time();
  
  file_header_.bucket_dir_offset = BM25SIndexFileHeader::HEADER_SIZE;
  file_header_.bucket_dir_size = NUM_BUCKETS * sizeof(BucketDirectoryEntry);
  file_header_.vocab_buckets_offset = file_header_.bucket_dir_offset + file_header_.bucket_dir_size;
  file_header_.vocab_buckets_size = total_vocab_bytes;
  file_header_.posting_data_offset = file_header_.vocab_buckets_offset + total_vocab_bytes;
  file_header_.posting_data_size = posting_data_size;
  
  // 4. 写入 Header 占位
  LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 2 - writing header");
  if (OB_FAIL(write_header_and_directory(fd))) {
    LOG_WARN("BM25S finalize_and_persist V2: fail to write header", K(ret));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  // 5. 第二遍：逐桶写入 Vocab Bucket + Posting Data
  LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 3 - writing data per bucket");
  
  int64_t vocab_write_offset = file_header_.vocab_buckets_offset;
  int64_t posting_write_offset = file_header_.posting_data_offset;
  int64_t current_vocab_offset = 0;
  int64_t written_postings = 0;
  
  for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
    bucket_directory_[bucket_id].offset = current_vocab_offset;
    
    if (bucket_vocab_sizes.at(bucket_id) == 0) {
      bucket_directory_[bucket_id].entry_count = 0;
      bucket_directory_[bucket_id].size = 0;
      continue;
    }
    
    // 重新读取桶数据
    ObArray<TmpRecord> records;
    char *bucket_buf = nullptr;  // 零拷贝：存储桶数据的缓冲区
    typedef common::hash::ObHashMap<common::ObString, ObArray<TmpRecord>*, common::hash::NoPthreadDefendMode> TokenPostingMap;
    TokenPostingMap token_postings;
    bool token_postings_created = false;
    int64_t bucket_size = bucket_vocab_bytes.at(bucket_id);
    
    // 使用 do-while(0) 确保清理代码总是执行
    do {
      if (OB_FAIL(read_variable_bucket(bucket_id, records, bucket_buf))) {
        LOG_WARN("BM25S finalize_and_persist V2: fail to read bucket", K(ret), K(bucket_id));
        break;
      }
      
      // 桶内小 HashMap 统计 df 和 posting 列表
      if (OB_FAIL(token_postings.create(1024, "BM25SBucketPost"))) {
        LOG_WARN("BM25S finalize_and_persist V2: fail to create token_postings", K(ret), K(bucket_id));
        break;
      }
      token_postings_created = true;
      
      // 按 token 分组
      for (int64_t i = 0; i < records.count(); ++i) {
        TmpRecord &rec = records.at(i);
        ObArray<TmpRecord> *posting_list = nullptr;
        int tmp_ret = token_postings.get_refactored(rec.token, posting_list);
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          // 新 token，创建 posting list
          posting_list = static_cast<ObArray<TmpRecord>*>(ob_malloc(sizeof(ObArray<TmpRecord>), "BM25SPostList"));
          if (OB_NOT_NULL(posting_list)) {
            new (posting_list) ObArray<TmpRecord>();
            token_postings.set_refactored(rec.token, posting_list, 0);
          }
        }
        if (OB_NOT_NULL(posting_list)) {
          posting_list->push_back(rec);
        }
      }
      
      // 构建 VocabEntry 列表：排序 + 取首尾获得 min/max_doc_id（合并为一个循环）
      ObArray<VocabEntryMem> entries;
      
      for (TokenPostingMap::iterator it = token_postings.begin(); it != token_postings.end(); ++it) {
        ObArray<TmpRecord> *pl = it->second;
        if (OB_ISNULL(pl) || pl->count() == 0) {
          continue;  // 跳过空的 posting list
        }
        
        // 先排序（只有 count > 1 才需要排序）
        if (pl->count() > 1) {
          std::sort(&pl->at(0), &pl->at(0) + pl->count(),
              [](const TmpRecord &a, const TmpRecord &b) { return a.doc_id < b.doc_id; });
        }
        
        VocabEntryMem entry;
        entry.token_hash = it->first.hash();
        entry.token = it->first;
        entry.posting_offset = 0;
        entry.posting_count = pl->count();
        // posting_list 已排序，直接取首尾获得 min/max_doc_id（O(1)）
        entry.min_doc_id = static_cast<uint32_t>(pl->at(0).doc_id);
        entry.max_doc_id = static_cast<uint32_t>(pl->at(pl->count() - 1).doc_id);
        entries.push_back(entry);
      }
      
      bucket_directory_[bucket_id].entry_count = entries.count();
      
      // 按 hash 排序
      if (entries.count() > 1) {
        std::sort(&entries.at(0), &entries.at(0) + entries.count(),
            [](const VocabEntryMem &a, const VocabEntryMem &b) {
                return a.token_hash < b.token_hash;
            });
      }
      
      // 计算 posting_offset
      int64_t bucket_posting_offset = posting_write_offset - file_header_.posting_data_offset;
      for (int64_t j = 0; j < entries.count(); ++j) {
        entries.at(j).posting_offset = bucket_posting_offset;
        bucket_posting_offset += entries.at(j).posting_count * 8;
      }
      
      // 写入 Vocab Bucket
      bucket_directory_[bucket_id].size = static_cast<int32_t>(bucket_size);
      
      char *vocab_buf = static_cast<char*>(ob_malloc(bucket_size, "BM25SVocab"));
      if (OB_ISNULL(vocab_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      
      char *p = vocab_buf;
      for (int64_t j = 0; j < entries.count(); ++j) {
        VocabEntryMem &e = entries.at(j);
        memcpy(p, &e.token_hash, 8); p += 8;
        uint16_t token_len = static_cast<uint16_t>(e.token.length());
        memcpy(p, &token_len, 2); p += 2;
        memcpy(p, e.token.ptr(), token_len); p += token_len;
        memcpy(p, &e.posting_offset, 8); p += 8;
        memcpy(p, &e.posting_count, 4); p += 4;
        memcpy(p, &e.min_doc_id, 4); p += 4;  // 写入 min_doc_id
        memcpy(p, &e.max_doc_id, 4); p += 4;  // 写入 max_doc_id
      }
      
      int64_t write_size = 0;
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
              fd.second_id_, vocab_buf, bucket_size, vocab_write_offset, write_size))) {
        LOG_WARN("BM25S finalize_and_persist V2: fail to write vocab bucket", K(ret), K(bucket_id));
        ob_free(vocab_buf);
        break;
      }
      ob_free(vocab_buf);
      
      // 写入 Posting Data
      for (int64_t j = 0; OB_SUCC(ret) && j < entries.count(); ++j) {
        VocabEntryMem &e = entries.at(j);
        ObArray<TmpRecord> *posting_list = nullptr;
        token_postings.get_refactored(e.token, posting_list);
        
        if (OB_ISNULL(posting_list) || posting_list->empty()) {
          continue;
        }
        
        // posting_list 已在构建 VocabEntry 前排序，无需再排
        int64_t df = posting_list->count();
        int64_t posting_buf_size = df * 8;
        char *posting_buf = static_cast<char*>(ob_malloc(posting_buf_size, "BM25SPosting"));
        if (OB_ISNULL(posting_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        
        uint32_t *doc_ids = reinterpret_cast<uint32_t*>(posting_buf);
        float *scores = reinterpret_cast<float*>(posting_buf + df * 4);
        
        for (int64_t k = 0; k < df; ++k) {
          const TmpRecord &rec = posting_list->at(k);
          doc_ids[k] = static_cast<uint32_t>(rec.doc_id);
          scores[k] = calc_bm25_score_f(rec.tf, rec.doc_len, df);
        }
        
        if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
                fd.second_id_, posting_buf, posting_buf_size, posting_write_offset, write_size))) {
          LOG_WARN("BM25S finalize_and_persist V2: fail to write posting", K(ret), K(bucket_id), K(j));
          ob_free(posting_buf);
          break;
        }
        
        posting_write_offset += posting_buf_size;
        written_postings += df;
        ob_free(posting_buf);
      }
      
      if (OB_SUCC(ret)) {
        vocab_write_offset += bucket_size;
        current_vocab_offset += bucket_size;
      }
    } while (0);
    
    // 【清理代码】无论成功失败都执行
    // 清理 posting 列表
    if (token_postings_created) {
      for (TokenPostingMap::iterator it = token_postings.begin(); it != token_postings.end(); ++it) {
        if (OB_NOT_NULL(it->second)) {
          it->second->~ObArray();
          ob_free(it->second);
        }
      }
      token_postings.destroy();
    }
    
    // 零拷贝优化：只需释放一个 bucket_buf，不需要逐个释放 token
    if (bucket_buf != nullptr) {
      ob_free(bucket_buf);
    }
    
    if (OB_FAIL(ret)) {
      break;  // 错误时退出外层循环
    }
    
    if ((bucket_id + 1) % 128 == 0) {
      LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 3 progress", 
               K(bucket_id), K(NUM_BUCKETS), K(written_postings));
    }
  }
  
  // 6. 更新 Header（写入 Bucket Directory）
  if (OB_SUCC(ret)) {
    LOG_INFO("BM25S finalize_and_persist V2 OPT: phase 4 - updating header");
    ret = update_header(fd);
  }
  
  // 7. 同步并关闭文件
  share::ObIODeviceLocalFileOp::fsync(fd);
  share::ObIODeviceLocalFileOp::close(fd);
  
  // 8. 清理 TmpFile
  cleanup_tmp_files();
  
  // 9. 清理收集阶段数据
  unique_docs_.reuse();
  
  if (OB_SUCC(ret)) {
    is_finalized_ = true;
    MEMCPY(index_file_path_, index_path, strlen(index_path) + 1);
    
    // 10. 保存 bucket_directory_ 到独立内存（因为 allocator_.reset() 会释放它）
    int64_t dir_size = NUM_BUCKETS * sizeof(BucketDirectoryEntry);
    BucketDirectoryEntry *saved_dir = static_cast<BucketDirectoryEntry*>(
        ob_malloc(dir_size, "BM25SBucketDir"));
    if (OB_ISNULL(saved_dir)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("BM25S finalize_and_persist: fail to alloc saved_dir", K(ret), K(dir_size));
      // 不 reset allocator_，保留原来的 bucket_directory_
    } else if (OB_NOT_NULL(bucket_directory_)) {
      MEMCPY(saved_dir, bucket_directory_, dir_size);
      
      // 11. 释放 ArenaAllocator 中的所有临时内存
      // 注意：主要内存(bucket_buf/vocab_buf/posting_buf)已改用 ob_malloc，在使用后即释放
      // allocator_ 中只剩余 ObArray 指针等小分配
      int64_t allocator_used = allocator_.total();
      LOG_INFO("BM25S finalize_and_persist: allocator memory before reset (small allocations only)", 
               K(allocator_used));
      
      allocator_.reset();
      
      // 12. 恢复 bucket_directory_ 指针
      bucket_directory_ = saved_dir;
      
      LOG_INFO("BM25S finalize_and_persist: allocator reset, memory freed");
    }
    
    // 重新打开文件供查询使用
    if (OB_FAIL(share::ObIODeviceLocalFileOp::open(index_path, O_RDONLY, 0, persist_fd_))) {
      LOG_WARN("BM25S finalize_and_persist: fail to reopen file for query", K(ret), K(index_path));
    } else {
      persist_file_opened_ = true;
      LOG_INFO("BM25S finalize_and_persist: file reopened for query", K(index_path));
    }
    
    int64_t end_time = ObTimeUtility::current_time();
    double elapsed_ms = (end_time - start_time) / 1000.0;
    
    LOG_INFO("BM25S finalize_and_persist V2 OPT: completed successfully", 
             K(index_path), K_(total_docs), K(total_vocab_size), 
             K(written_postings), K(posting_data_size),
             "elapsed_ms", elapsed_ms);
  }
  
  return ret;
}

// V2: write_vocab_buckets_streaming 已集成到 finalize_and_persist 中，不再需要单独函数

int ObFtsBM25SIndex::load_from_disk(const char *index_dir)
{
  int ret = OB_SUCCESS;
  
  // 如果已经成功从磁盘加载过，直接返回
  if (persist_file_opened_) {
    LOG_DEBUG("BM25S load_from_disk: already loaded, skip");
    return OB_SUCCESS;
  }
  
  if (OB_ISNULL(index_dir)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S load_from_disk: index_dir is null", K(ret));
    return ret;
  }
  
  // 1. 构建文件路径
  char file_path[512];
  snprintf(file_path, sizeof(file_path), "%s/bm25s_index.dat", index_dir);
  
  LOG_INFO("BM25S load_from_disk: starting", K(file_path));
  
  // 2. 检查文件是否存在
  bool is_exist = false;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::exist(file_path, is_exist))) {
    LOG_WARN("BM25S load_from_disk: fail to check file exist", K(ret), K(file_path));
    return ret;
  }
  if (!is_exist) {
    ret = OB_FILE_NOT_EXIST;
    LOG_INFO("BM25S load_from_disk: index file not exist", K(ret), K(file_path));
    return ret;
  }
  
  // 3. 打开文件
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(file_path, O_RDONLY, 0, persist_fd_))) {
    LOG_WARN("BM25S load_from_disk: fail to open file", K(ret), K(file_path));
    return ret;
  }
  
  // 4. 读取 Header
  int64_t read_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, &file_header_,
                                                 sizeof(file_header_), 0, read_size))) {
    LOG_WARN("BM25S load_from_disk: fail to read header", K(ret));
    close_index_file();
    return ret;
  }
  
  // 5. 验证 Header
  if (!file_header_.is_valid()) {
    ret = OB_INVALID_DATA;
    LOG_WARN("BM25S load_from_disk: invalid header", K(ret), 
             K(file_header_.magic), K(file_header_.version));
    close_index_file();
    return ret;
  }
  
  // 6. 加载 Bucket Directory（使用 ob_malloc，与 close_index_file 中的 ob_free 对应）
  int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  bucket_directory_ = static_cast<BucketDirectoryEntry*>(ob_malloc(dir_size, "BM25SBucketDir"));
  if (OB_ISNULL(bucket_directory_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S load_from_disk: fail to alloc bucket_directory", K(ret), K(dir_size));
    close_index_file();
    return ret;
  }
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, bucket_directory_,
                                                 dir_size, file_header_.bucket_dir_offset,
                                                 read_size))) {
    LOG_WARN("BM25S load_from_disk: fail to read bucket directory", K(ret));
    close_index_file();
    return ret;
  }
  
  // V2: 不需要加载 Vocab Buckets 到内存
  // 查询时直接通过 find_token_in_file() 从文件按需读取
  // 这样大幅减少内存占用（从 ~1GB 降低到仅加载 Header + Bucket Directory）
  
  // 7. 恢复统计信息
  if (OB_SUCC(ret)) {
    total_docs_ = file_header_.total_docs;
    avg_doc_len_ = file_header_.avg_doc_len;
    
    // 保存文件路径
    MEMCPY(index_file_path_, file_path, strlen(file_path) + 1);
    persist_file_opened_ = true;
    is_inited_ = true;
    is_finalized_ = true;  // 从文件加载视为已 finalize
    
    LOG_INFO("BM25S load_from_disk: completed", K(file_path),
             K(file_header_.total_docs), K(file_header_.vocab_size),
             K(file_header_.total_postings));
  } else {
    close_index_file();
  }
  
  return ret;
}

int ObFtsBM25SIndex::ensure_ready()
{
  int ret = OB_SUCCESS;
  
  // Double-checked locking: 先无锁检查，避免不必要的锁开销
  if (persist_file_opened_) {
    LOG_DEBUG("BM25S ensure_ready: index file already opened");
    return OB_SUCCESS;
  }
  
  // 加锁再检查一次（防止多线程重复加载）
  SpinWLockGuard guard(lock_);
  
  // 再次检查（可能其他线程已经完成加载）
  if (persist_file_opened_) {
    LOG_DEBUG("BM25S ensure_ready: index already loaded by another thread");
    return OB_SUCCESS;
  }
  
  // 尝试从默认路径加载索引文件
  const char *default_index_dir = "/data/obcluster/store/bm25s_index";
  
  LOG_INFO("BM25S ensure_ready: attempting to load index from disk",
           K(default_index_dir));
  
  ret = load_from_disk(default_index_dir);
  
  if (OB_SUCC(ret)) {
    LOG_INFO("BM25S ensure_ready: index loaded successfully from disk",
             K_(index_file_path),
             "total_docs", file_header_.total_docs,
             "vocab_size", file_header_.vocab_size);
    
    // 初始化 KVCache（如果还没初始化）
    if (!is_bm25s_cache_inited()) {
      int cache_ret = init_bm25s_cache();
      if (OB_SUCCESS != cache_ret) {
        LOG_WARN("BM25S ensure_ready: fail to init KVCache (non-fatal)", K(cache_ret));
      }
    }
  } else {
    LOG_WARN("BM25S ensure_ready: fail to load index from disk",
             K(ret), K(default_index_dir));
  }
  
  return ret;
}

void ObFtsBM25SIndex::close_index_file()
{
  if (persist_file_opened_ && persist_fd_.is_normal_file()) {
    share::ObIODeviceLocalFileOp::close(persist_fd_);
    persist_file_opened_ = false;
  }
  if (OB_NOT_NULL(bucket_directory_)) {
    ob_free(bucket_directory_);  // 使用 ob_free（对应 ob_malloc）
    bucket_directory_ = nullptr;
  }
}

// ========================================================================
// Phase 3: KVCache 全局缓存管理
// ========================================================================

// 全局缓存实例（单例，一级）
static ObBM25SPostingCache *g_posting_cache = nullptr;
static bool g_cache_inited = false;

int init_bm25s_cache()
{
  int ret = OB_SUCCESS;
  
  if (g_cache_inited) {
    LOG_INFO("BM25S KVCache: already initialized");
    return OB_SUCCESS;
  }
  
  LOG_INFO("BM25S KVCache: initializing PostingCache");
  
  // 分配缓存对象
  void *posting_buf = ob_malloc(sizeof(ObBM25SPostingCache), "BM25SCache");
  
  if (OB_ISNULL(posting_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S KVCache: fail to alloc PostingCache", K(ret));
    return ret;
  }
  
  g_posting_cache = new (posting_buf) ObBM25SPostingCache();
  
  // 初始化缓存
  // 参数：cache_name, priority (1-10, 越高优先级越高)
  if (OB_FAIL(g_posting_cache->init("bm25s_posting_cache", 1))) {
    LOG_WARN("BM25S KVCache: fail to init PostingCache", K(ret));
    g_posting_cache->~ObBM25SPostingCache();
    ob_free(posting_buf);
    g_posting_cache = nullptr;
    return ret;
  }
  
  g_cache_inited = true;
  LOG_INFO("BM25S KVCache: PostingCache initialized successfully");
  
  return ret;
}

void destroy_bm25s_cache()
{
  LOG_INFO("BM25S KVCache: destroying PostingCache");
  
  if (g_posting_cache) {
    g_posting_cache->destroy();
    g_posting_cache->~ObBM25SPostingCache();
    ob_free(g_posting_cache);
    g_posting_cache = nullptr;
  }
  g_cache_inited = false;
  
  LOG_INFO("BM25S KVCache: PostingCache destroyed");
}

bool is_bm25s_cache_inited()
{
  return g_cache_inited;
}

ObBM25SPostingCache* get_bm25s_posting_cache()
{
  return g_posting_cache;
}

// ========================================================================
// V3: Posting LRU ObHashMap 实现
// ========================================================================

void ObFtsBM25SIndex::posting_lru_add_to_head(PostingLRUNode *node)
{
  if (OB_ISNULL(node)) {
    return;
  }
  node->prev = &posting_lru_head_;
  node->next = posting_lru_head_.next;
  posting_lru_head_.next->prev = node;
  posting_lru_head_.next = node;
}

void ObFtsBM25SIndex::posting_lru_remove_node(PostingLRUNode *node)
{
  if (OB_ISNULL(node) || OB_ISNULL(node->prev) || OB_ISNULL(node->next)) {
    return;
  }
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = nullptr;
  node->next = nullptr;
}

void ObFtsBM25SIndex::posting_lru_evict_oldest()
{
  PostingLRUNode *oldest = posting_lru_tail_.prev;
  if (oldest == &posting_lru_head_) {
    return;
  }
  posting_lru_remove_node(oldest);
  posting_lru_map_.erase_refactored(oldest->key);
  posting_lru_size_--;
  // 内存由 posting_lru_allocator_ 管理，延迟统一释放
}

int ObFtsBM25SIndex::init_posting_lru()
{
  int ret = OB_SUCCESS;
  if (posting_lru_inited_) {
    return OB_SUCCESS;
  }

  common::SpinWLockGuard guard(posting_lru_lock_);
  if (posting_lru_inited_) {
    return OB_SUCCESS;
  }

  int64_t bucket_cnt = posting_lru_capacity_ > 0 ? posting_lru_capacity_ * 2 : DEFAULT_POSTING_LRU_CAPACITY * 2;
  if (bucket_cnt <= 0) {
    bucket_cnt = DEFAULT_POSTING_LRU_CAPACITY * 2;
  }

  if (OB_FAIL(posting_lru_map_.create(bucket_cnt, "BM25SLRU", "BM25SLRU"))) {
    LOG_WARN("BM25S posting LRU: fail to create hashmap", K(ret), K(bucket_cnt));
  } else {
    posting_lru_head_.next = &posting_lru_tail_;
    posting_lru_head_.prev = nullptr;
    posting_lru_tail_.prev = &posting_lru_head_;
    posting_lru_tail_.next = nullptr;
    posting_lru_size_ = 0;
    posting_lru_inited_ = true;
    LOG_INFO("BM25S posting LRU: initialized", K(bucket_cnt), K_(posting_lru_capacity));
  }

  return ret;
}

// ========================================================================
// Phase 3: KVCache 查询实现
// ========================================================================

int ObFtsBM25SIndex::find_token_in_file(const common::ObString &token,
                                         int64_t &posting_offset,
                                         int64_t &posting_count,
                                         uint32_t &min_doc_id,
                                         uint32_t &max_doc_id)
{
  int ret = OB_SUCCESS;
  min_doc_id = UINT32_MAX;
  max_doc_id = 0;
  
  if (!persist_file_opened_) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S find_token_in_file: index file not opened", K(ret));
    return ret;
  }
  
  // 计算 token 的 hash（必须与 write_vocab_buckets_streaming 一致，使用 ObString::hash()）
  uint64_t token_hash = token.hash();
  
  // 1. 计算桶 ID（与 Phase 2 写入时一致）
  int32_t bucket_id = static_cast<int32_t>(token_hash % file_header_.bucket_num);
  BucketDirectoryEntry &bucket = bucket_directory_[bucket_id];
  
  if (bucket.entry_count == 0) {
    LOG_DEBUG("BM25S find_token_in_file: bucket is empty", 
              K(token), K(bucket_id), K(token_hash), 
              K(file_header_.bucket_num), K(file_header_.vocab_size));
    return OB_ENTRY_NOT_EXIST;
  }
  
  // 2. 读取整个桶数据
  int64_t bucket_file_offset = file_header_.vocab_buckets_offset + bucket.offset;
  char *bucket_data = static_cast<char*>(cache_allocator_.alloc(bucket.size));
  if (OB_ISNULL(bucket_data)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S find_token_in_file: fail to alloc bucket_data", K(ret), K(bucket.size));
    return ret;
  }
  
  int64_t read_size = 0;
  ret = share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, bucket_data, 
                                          bucket.size, bucket_file_offset, read_size);
  if (OB_FAIL(ret)) {
    LOG_WARN("BM25S find_token_in_file: fail to read bucket", 
             K(ret), K(bucket_id), K(bucket_file_offset));
    cache_allocator_.free(bucket_data);
    return ret;
  }
  
  // 3. 遍历桶内 Entry 查找 token
  // 格式: [hash:8][len:2][token:N][offset:8][count:4][min_doc_id:4][max_doc_id:4] = 30+N bytes
  ret = OB_ENTRY_NOT_EXIST;
  char *p = bucket_data;
  char *end = bucket_data + bucket.size;
  
  while (p < end) {
    uint64_t entry_hash = *reinterpret_cast<uint64_t*>(p); p += 8;
    uint16_t entry_token_len = *reinterpret_cast<uint16_t*>(p); p += 2;
    const char *entry_token_data = p; p += entry_token_len;
    int64_t entry_posting_offset = *reinterpret_cast<int64_t*>(p); p += 8;
    int32_t entry_posting_count = *reinterpret_cast<int32_t*>(p); p += 4;
    uint32_t entry_min_doc_id = *reinterpret_cast<uint32_t*>(p); p += 4;  // 读取 min_doc_id
    uint32_t entry_max_doc_id = *reinterpret_cast<uint32_t*>(p); p += 4;  // 读取 max_doc_id
    
    // 比较 token（先比 hash，再比内容）
    if (entry_hash == token_hash &&
        entry_token_len == token.length() &&
        0 == MEMCMP(entry_token_data, token.ptr(), token.length())) {
      posting_offset = entry_posting_offset;
      posting_count = entry_posting_count;
      min_doc_id = entry_min_doc_id;
      max_doc_id = entry_max_doc_id;
      ret = OB_SUCCESS;
      LOG_DEBUG("BM25S find_token_in_file: token found", 
                K(token), K(bucket_id), K(posting_offset), K(posting_count), K(min_doc_id), K(max_doc_id));
      break;
    }
    
    // 优化：桶内按 hash 排序，如果 entry_hash > token_hash 可以提前退出
    if (entry_hash > token_hash) {
      break;
    }
  }
  
  if (ret != OB_SUCCESS) {
    LOG_INFO("BM25S find_token: NOT FOUND", K(token), K(bucket_id));
  }
  
  cache_allocator_.free(bucket_data);
  return ret;
}

int ObFtsBM25SIndex::load_posting_to_cache(const common::ObString &token,
                                            const ObBM25SPostingValue *&posting,
                                            common::ObKVCacheHandle &handle)
{
  int ret = OB_SUCCESS;
  
  // 热路径，移除日志
  
  // 1. 从文件查找 token 的 posting 信息
  int64_t posting_offset = 0;
  int64_t posting_count = 0;
  uint32_t min_doc_id = UINT32_MAX;
  uint32_t max_doc_id = 0;  // 预留，暂不使用
  
  if (OB_FAIL(find_token_in_file(token, posting_offset, posting_count, min_doc_id, max_doc_id))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // 【负缓存】token 在磁盘索引中不存在，缓存空 posting（热路径，移除日志）
      ObBM25SPostingValue empty_value;
      empty_value.set(0, nullptr, nullptr);  // count = 0 表示不存在
      ObBM25SPostingKey key(token);
      g_posting_cache->put_and_fetch(key, empty_value, posting, handle);
      // 仍然返回 OB_ENTRY_NOT_EXIST
    } else {
      LOG_WARN("BM25S load_posting_to_cache: fail to find token in file", K(ret), K(token));
    }
    return ret;
  }
  
  // 注意：不在加载时做前过滤，总是加载到缓存
  // 前过滤在缓存命中后通过 doc_ids[0] 检查（避免重复读 vocab）
  
  if (posting_count == 0) {
    // 【负缓存】token 存在但没有 posting，缓存空 posting（热路径，移除日志）
    ObBM25SPostingValue empty_value;
    empty_value.set(0, nullptr, nullptr);
    ObBM25SPostingKey key(token);
    g_posting_cache->put_and_fetch(key, empty_value, posting, handle);
    ret = OB_ENTRY_NOT_EXIST;
    return ret;
  }
  
  // 2. 计算数据大小和文件偏移
  int64_t doc_ids_size = posting_count * sizeof(uint32_t);
  int64_t scores_size = posting_count * sizeof(float);
  int64_t total_size = doc_ids_size + scores_size;
  int64_t file_offset = file_header_.posting_data_offset + posting_offset;
  
  // 3. 分配临时缓冲区
  char *buf = static_cast<char*>(cache_allocator_.alloc(total_size));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S load_posting_to_cache: fail to alloc buffer", K(ret), K(total_size));
    return ret;
  }
  
  // 4. 从文件读取
  int64_t read_size = 0;
  ret = share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, buf, total_size,
                                          file_offset, read_size);
  if (OB_FAIL(ret)) {
    LOG_WARN("BM25S load_posting_to_cache: fail to read posting data", 
             K(ret), K(token), K(file_offset), K(total_size));
    cache_allocator_.free(buf);
    return ret;
  }
  
  // 5. 构建临时 Value
  ObBM25SPostingValue tmp_value;
  uint32_t *doc_ids = reinterpret_cast<uint32_t*>(buf);
  float *scores = reinterpret_cast<float*>(buf + doc_ids_size);
  tmp_value.set(posting_count, doc_ids, scores);
  
  // 6. 放入缓存（put_and_fetch 会调用 deep_copy 并返回缓存中的指针）
  ObBM25SPostingKey key(token);
  
  ret = g_posting_cache->put_and_fetch(key, tmp_value, posting, handle);
  // 热路径，移除成功/失败日志
  
  // 7. 释放临时缓冲区（数据已被 deep_copy 到 KVCache）
  cache_allocator_.free(buf);
  
  return ret;
}

int ObFtsBM25SIndex::get_posting_from_lru(const common::ObString &token,
                                           const ObBM25SPostingValue *&posting)
{
  int ret = OB_SUCCESS;
  posting = nullptr;

  if (!use_posting_hashmap_) {
    common::ObKVCacheHandle handle;
    return get_posting_from_cache(token, posting, handle);
  }

  if (!persist_file_opened_) {
    if (OB_FAIL(ensure_ready())) {
      return ret;
    }
  }

  if (!persist_file_opened_) {
    return common::OB_NOT_INIT;
  }

  if (!posting_lru_inited_) {
    if (OB_FAIL(init_posting_lru())) {
      return ret;
    }
  }

  uint64_t token_hash = token.hash();
  PostingHashKey lookup_key(token_hash, static_cast<int32_t>(token.length()), token.ptr());

  {
    common::SpinRLockGuard guard(posting_lru_lock_);
    PostingLRUNode *node = nullptr;
    ret = posting_lru_map_.get_refactored(lookup_key, node);
    if (common::OB_SUCCESS == ret && OB_NOT_NULL(node)) {
      posting = node->posting;
      if (OB_ISNULL(posting)) {
        return common::OB_ERR_UNEXPECTED;
      }
      if (posting->count() == 0) {
        return common::OB_ENTRY_NOT_EXIST;
      }
      // 前过滤暂时禁用，测试性能影响
      int64_t id_limit = g_fts_generic_filter_ctx.get_min_upper_bound();
      if (id_limit < INT64_MAX && posting->get_doc_id(0) >= static_cast<uint32_t>(id_limit)) {
        return common::OB_ENTRY_NOT_EXIST;
      }
      return common::OB_SUCCESS;
    } else if (common::OB_HASH_NOT_EXIST != ret) {
      return ret;
    }
  }

  ret = load_posting_to_lru(token, posting, token_hash);
  return ret;
}

int ObFtsBM25SIndex::load_posting_to_lru(const common::ObString &token,
                                          const ObBM25SPostingValue *&posting,
                                          uint64_t token_hash)
{
  int ret = OB_SUCCESS;
  posting = nullptr;

  if (!persist_file_opened_) {
    if (OB_FAIL(ensure_ready())) {
      return ret;
    }
  }

  if (!posting_lru_inited_) {
    if (OB_FAIL(init_posting_lru())) {
      return ret;
    }
  }

  if (token_hash == 0) {
    token_hash = token.hash();
  }

  bool disk_not_exist = false;
  int64_t posting_offset = 0;
  int64_t posting_count = 0;
  uint32_t min_doc_id = UINT32_MAX;
  uint32_t max_doc_id = 0;  // 预留，暂不使用
  char *tmp_buf = nullptr;
  int64_t tmp_buf_size = 0;

  if (OB_FAIL(find_token_in_file(token, posting_offset, posting_count, min_doc_id, max_doc_id))) {
    if (common::OB_ENTRY_NOT_EXIST == ret) {
      disk_not_exist = true;
      ret = OB_SUCCESS;
    } else {
      return ret;
    }
  }

  // 注意：不在加载时做前过滤，总是加载到缓存
  // 前过滤在缓存命中后通过 doc_ids[0] 检查（避免重复读 vocab）

  if (!disk_not_exist && posting_count > 0) {
    tmp_buf_size = posting_count * (sizeof(uint32_t) + sizeof(float));
    tmp_buf = static_cast<char*>(ob_malloc(tmp_buf_size, "BM25SLRUTmp"));
    if (OB_ISNULL(tmp_buf)) {
      LOG_WARN("BM25S posting LRU: fail to alloc temp buffer", K(tmp_buf_size));
      return common::OB_ALLOCATE_MEMORY_FAILED;
    }
    int64_t read_size = 0;
    int64_t file_offset = file_header_.posting_data_offset + posting_offset;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(
            persist_fd_.second_id_, tmp_buf, tmp_buf_size, file_offset, read_size))) {
      LOG_WARN("BM25S posting LRU: fail to read posting data", K(ret), K(token), K(file_offset), K(tmp_buf_size));
      ob_free(tmp_buf);
      return ret;
    }
  }

  PostingHashKey lookup_key(token_hash, static_cast<int32_t>(token.length()), token.ptr());

  common::SpinWLockGuard guard(posting_lru_lock_);
  PostingLRUNode *existing_node = nullptr;
  int map_ret = posting_lru_map_.get_refactored(lookup_key, existing_node);
  if (common::OB_SUCCESS == map_ret && OB_NOT_NULL(existing_node)) {
    posting = existing_node->posting;
    ob_free(tmp_buf);
    return posting->count() == 0 ? common::OB_ENTRY_NOT_EXIST : common::OB_SUCCESS;
  } else if (common::OB_SUCCESS != map_ret && common::OB_HASH_NOT_EXIST != map_ret) {
    ob_free(tmp_buf);
    return map_ret;
  }

  while (posting_lru_size_ >= posting_lru_capacity_) {
    posting_lru_evict_oldest();
  }

  PostingLRUNode *new_node = static_cast<PostingLRUNode*>(
      posting_lru_allocator_.alloc(sizeof(PostingLRUNode)));
  ObBM25SPostingValue *new_posting = static_cast<ObBM25SPostingValue*>(
      posting_lru_allocator_.alloc(sizeof(ObBM25SPostingValue)));
  char *token_buf = nullptr;
  if (token.length() > 0) {
    token_buf = static_cast<char*>(posting_lru_allocator_.alloc(token.length()));
  }

  if (OB_ISNULL(new_node) || OB_ISNULL(new_posting) || (token.length() > 0 && OB_ISNULL(token_buf))) {
    LOG_WARN("BM25S posting LRU: alloc node/posting/token failed", K(token.length()));
    ob_free(tmp_buf);
    return common::OB_ALLOCATE_MEMORY_FAILED;
  }

  MEMCPY(token_buf, token.ptr(), token.length());
  new (new_node) PostingLRUNode();
  new (new_posting) ObBM25SPostingValue();
  new_node->token.assign_ptr(token_buf, token.length());
  new_node->key = PostingHashKey(token_hash, static_cast<int32_t>(token.length()), token_buf);

  if (!disk_not_exist && posting_count > 0) {
    int64_t doc_ids_size = posting_count * sizeof(uint32_t);
    int64_t scores_size = posting_count * sizeof(float);
    uint32_t *doc_ids_buf = reinterpret_cast<uint32_t*>(posting_lru_allocator_.alloc(doc_ids_size));
    float *scores_buf = reinterpret_cast<float*>(posting_lru_allocator_.alloc(scores_size));
    if (OB_ISNULL(doc_ids_buf) || OB_ISNULL(scores_buf)) {
      LOG_WARN("BM25S posting LRU: alloc posting data buffer failed", K(doc_ids_size), K(scores_size));
      ob_free(tmp_buf);
      return common::OB_ALLOCATE_MEMORY_FAILED;
    }
    MEMCPY(doc_ids_buf, tmp_buf, doc_ids_size);
    MEMCPY(scores_buf, tmp_buf + doc_ids_size, scores_size);
    new_posting->set(posting_count, doc_ids_buf, scores_buf);
  } else {
    new_posting->set(0, nullptr, nullptr);
  }

  new_node->posting = new_posting;

  if (OB_FAIL(posting_lru_map_.set_refactored(new_node->key, new_node))) {
    if (common::OB_HASH_EXIST == ret) {
      if (OB_SUCC(posting_lru_map_.get_refactored(lookup_key, existing_node))) {
        posting = existing_node->posting;
        ret = posting->count() == 0 ? common::OB_ENTRY_NOT_EXIST : common::OB_SUCCESS;
      }
    } else {
      LOG_WARN("BM25S posting LRU: fail to insert hashmap", K(ret), K(token));
    }
  } else {
    posting_lru_add_to_head(new_node);
    posting_lru_size_++;
    posting = new_posting;
    ret = (!disk_not_exist && posting_count > 0) ? common::OB_SUCCESS : common::OB_ENTRY_NOT_EXIST;
  }

  ob_free(tmp_buf);
  return ret;
}

int ObFtsBM25SIndex::get_posting_from_cache(const common::ObString &token,
                                             const ObBM25SPostingValue *&posting,
                                             common::ObKVCacheHandle &handle)
{
  int ret = OB_SUCCESS;
  if (use_posting_hashmap_) {
    return get_posting_from_lru(token, posting);
  }
  
  // 确保索引文件已打开（如果未打开，尝试从磁盘加载）
  if (!persist_file_opened_) {
    LOG_DEBUG("BM25S get_posting_from_cache: index file not opened, trying ensure_ready");
    ret = ensure_ready();
    if (OB_FAIL(ret)) {
      LOG_WARN("BM25S get_posting_from_cache: fail to ensure index ready", K(ret));
      return ret;
    }
  }
  
  // 再次检查（ensure_ready 可能加载了文件）
  if (!persist_file_opened_) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S get_posting_from_cache: index file still not opened after ensure_ready", K(ret));
    return ret;
  }
  
  // 确保 KVCache 已初始化
  if (!g_cache_inited || OB_ISNULL(g_posting_cache)) {
    LOG_DEBUG("BM25S get_posting_from_cache: cache not initialized, initializing now");
    ret = init_bm25s_cache();
    if (OB_FAIL(ret)) {
      LOG_WARN("BM25S get_posting_from_cache: fail to init cache", K(ret));
      return ret;
    }
  }
  
  // 1. 尝试从缓存获取（Key 使用 token 字符串）
  ObBM25SPostingKey key(token);
  
  ret = g_posting_cache->get(key, posting, handle);
  
  if (OB_SUCCESS == ret) {
    // 缓存命中，检查是否为负缓存（count == 0）
    if (posting->count() == 0) {
      // 负缓存命中：token 不存在（热路径，移除日志）
      return OB_ENTRY_NOT_EXIST;
    }
    // 前过滤暂时禁用，测试性能影响
    // int64_t id_limit = g_fts_generic_filter_ctx.get_min_upper_bound();
    // if (id_limit < INT64_MAX && posting->get_doc_id(0) >= static_cast<uint32_t>(id_limit)) {
    //   return OB_ENTRY_NOT_EXIST;
    // }
    return OB_SUCCESS;
  }
  
  if (OB_ENTRY_NOT_EXIST == ret) {
    // 2. 缓存未命中，从文件加载（热路径，移除日志）
    ret = load_posting_to_cache(token, posting, handle);
  }
  
  return ret;
}

int ObFtsBM25SIndex::search_with_cache(const common::ObIArray<common::ObString> &tokens,
                                        int64_t id_limit,
                                        int64_t top_k,
                                        common::ObIArray<DocScore> &results)
{
  int ret = OB_SUCCESS;
  int64_t start_time = ObTimeUtility::current_time();
  
  LOG_INFO("BM25S search_with_cache: starting",
           "token_count", tokens.count(), K(id_limit), K(top_k));
  
  if (!persist_file_opened_) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S search_with_cache: index not loaded", K(ret));
    return ret;
  }
  
  if (tokens.empty() || top_k <= 0) {
    LOG_INFO("BM25S search_with_cache: empty query or invalid top_k");
    return OB_SUCCESS;
  }
  
  // 1. 初始化缓存（如果还没初始化）
  if (!g_cache_inited) {
    if (OB_FAIL(init_bm25s_cache())) {
      LOG_WARN("BM25S search_with_cache: fail to init cache", K(ret));
      return ret;
    }
  }
  
  // 2. 文档得分累加器
  common::hash::ObHashMap<int64_t, double, common::hash::NoPthreadDefendMode> doc_scores;
  if (OB_FAIL(doc_scores.create(1024, "BM25SSearch"))) {
    LOG_WARN("BM25S search_with_cache: fail to create doc_scores map", K(ret));
    return ret;
  }
  
  // 3. 遍历每个 token（一级缓存：直接 token → posting）
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;
  int64_t tokens_not_found = 0;
  
  for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
    const common::ObString &token = tokens.at(i);
    
    if (token.empty()) {
      continue;
    }
    
    // 获取 posting（从缓存或文件）
    const ObBM25SPostingValue *posting = nullptr;
    common::ObKVCacheHandle handle;
    
    int posting_ret = get_posting_from_cache(token, posting, handle);
    if (OB_ENTRY_NOT_EXIST == posting_ret) {
      // 未知词，跳过
      tokens_not_found++;
      continue;
    } else if (OB_FAIL(posting_ret)) {
      LOG_WARN("BM25S search_with_cache: fail to get posting", K(posting_ret), K(token));
      continue;
    }
    
    // 统计缓存命中
    // 注：这里简单统计，实际上 get_posting_from_cache 内部会区分 hit/miss
    
    // 累加得分
    int64_t valid_count = posting->get_valid_count(id_limit);
    for (int64_t j = 0; j < valid_count; ++j) {
      int64_t doc_id = posting->get_doc_id(j);
      double score = posting->get_score(j);
      
      double existing = 0.0;
      doc_scores.get_refactored(doc_id, existing);
      doc_scores.set_refactored(doc_id, existing + score, 1 /*overwrite*/);
    }
    
    // handle 析构时自动释放缓存引用
  }
  
  // 4. Top-K 排序（使用最小堆）
  if (OB_SUCC(ret)) {
    // 定义比较函数用于最小堆
    auto cmp = [](const DocScore &a, const DocScore &b) {
      return a.score > b.score;  // 最小堆：分数小的在堆顶
    };
    std::priority_queue<DocScore, std::vector<DocScore>, decltype(cmp)> min_heap(cmp);
    
    for (auto it = doc_scores.begin(); OB_SUCC(ret) && it != doc_scores.end(); ++it) {
      DocScore ds(it->first, it->second);
      
      if (static_cast<int64_t>(min_heap.size()) < top_k) {
        min_heap.push(ds);
      } else if (ds.score > min_heap.top().score) {
        min_heap.pop();
        min_heap.push(ds);
      }
    }
    
    // 导出结果（按分数降序）
    std::vector<DocScore> tmp;
    tmp.reserve(min_heap.size());
    while (!min_heap.empty()) {
      tmp.push_back(min_heap.top());
      min_heap.pop();
    }
    
    for (int64_t i = tmp.size() - 1; i >= 0 && OB_SUCC(ret); --i) {
      if (OB_FAIL(results.push_back(tmp[i]))) {
        LOG_WARN("BM25S search_with_cache: fail to push result", K(ret));
        break;
      }
    }
  }
  
  doc_scores.destroy();
  
  int64_t end_time = ObTimeUtility::current_time();
  double elapsed_ms = (end_time - start_time) / 1000.0;
  
  LOG_INFO("BM25S search_with_cache: completed",
           "token_count", tokens.count(),
           K(id_limit),
           K(top_k),
           "result_count", results.count(),
           "tokens_not_found", tokens_not_found,
           "elapsed_ms", elapsed_ms);
  
  return ret;
}

void ObFtsBM25SIndex::print_cache_stats()
{
  if (g_posting_cache && g_cache_inited) {
    // 使用系统租户 ID（全局单例模式）
    uint64_t tenant_id = OB_SYS_TENANT_ID;
    int64_t cache_size = g_posting_cache->size(tenant_id);
    int64_t cache_count = g_posting_cache->count(tenant_id);
    int64_t hit_cnt = g_posting_cache->get_hit_cnt(tenant_id);
    int64_t miss_cnt = g_posting_cache->get_miss_cnt(tenant_id);
    double hit_rate = g_posting_cache->get_hit_rate(tenant_id);
    
    LOG_INFO("BM25S PostingCache stats",
             K(cache_size),
             K(cache_count),
             K(hit_cnt),
             K(miss_cnt),
             K(hit_rate));
  } else {
    LOG_INFO("BM25S PostingCache: not initialized");
  }
}

} // end namespace sql
} // end namespace oceanbase
