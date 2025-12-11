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
    index_fd_(-1),
    index_persisted_(false),
    collect_token_to_id_(),
    unique_docs_(),
    total_doc_len_(0),
    raw_entry_count_(0),
    scores_(),
    doc_ids_(),
    token_ptr_(),
    token_to_id_(),
    total_docs_(0),
    avg_doc_len_(0.0),
    allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "BM25SIndex")),
    lock_()
{
  // 初始化 TmpFile 相关数组
  for (int64_t i = 0; i < NUM_BUCKETS; i++) {
    bucket_fds_[i] = -1;
    bucket_write_offsets_[i] = 0;
    bucket_entry_counts_[i] = 0;
  }
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
      if (OB_FAIL(collect_token_to_id_.create(DEFAULT_BUCKET_COUNT,
                                    ObMemAttr(OB_SERVER_TENANT_ID, "BM25SCollectMap")))) {
        LOG_WARN("BM25S init: fail to create collect token map", K(ret));
      } else if (OB_FAIL(unique_docs_.create(DEFAULT_BUCKET_COUNT,
                                              ObMemAttr(OB_SERVER_TENANT_ID, "BM25SDocSet")))) {
        LOG_WARN("BM25S init: fail to create unique docs set", K(ret));
      } else if (OB_FAIL(token_to_id_.create(DEFAULT_BUCKET_COUNT,
                                              ObMemAttr(OB_SERVER_TENANT_ID, "BM25STokenMap")))) {
        LOG_WARN("BM25S init: fail to create token to id map", K(ret));
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

int ObFtsBM25SIndex::write_to_bucket(int64_t bucket_id, const FileRecord &record)
{
  int ret = OB_SUCCESS;
  
  // 参数检查
  if (bucket_id < 0 || bucket_id >= NUM_BUCKETS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S write_to_bucket: invalid bucket id", K(ret), K(bucket_id), K(NUM_BUCKETS));
    return ret;
  }
  
  if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S write_to_bucket: tmp file not initialized", 
             K(ret), K(bucket_id), K_(tmp_files_inited), K(bucket_fds_[bucket_id]));
    return ret;
  }
  
  // 构建 IO 信息
  tmp_file::ObTmpFileIOInfo io_info;
  io_info.fd_ = bucket_fds_[bucket_id];
  io_info.dir_id_ = tmp_dir_id_;
  io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);  // 使用 DATA_READ（写入也适用）
  io_info.buf_ = reinterpret_cast<char*>(const_cast<FileRecord*>(&record));
  io_info.size_ = sizeof(FileRecord);
  io_info.io_timeout_ms_ = 5000;  // 5 秒超时
  
  // 写入（append 模式，TmpFile 自动追加）
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S write_to_bucket: tmp file manager is null", K(ret));
    return ret;
  }
  
  if (OB_FAIL(tmp_mgr->write(MTL_ID(), io_info))) {
    LOG_WARN("BM25S write_to_bucket: fail to write to tmp file", 
             K(ret), K(bucket_id), K(sizeof(FileRecord)), K(bucket_fds_[bucket_id]));
  } else {
    // 更新写入偏移（用于统计）
    ATOMIC_AAF(&bucket_write_offsets_[bucket_id], sizeof(FileRecord));
  }
  
  return ret;
}

int ObFtsBM25SIndex::write_records_to_bucket(int64_t bucket_id, 
                                              const FileRecord *records, 
                                              int64_t count)
{
  int ret = OB_SUCCESS;
  
  if (count <= 0 || OB_ISNULL(records)) {
    return OB_SUCCESS;
  }
  
  // 参数检查
  if (bucket_id < 0 || bucket_id >= NUM_BUCKETS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S write_records_to_bucket: invalid bucket id", K(ret), K(bucket_id), K(NUM_BUCKETS));
    return ret;
  }
  
  if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S write_records_to_bucket: tmp file not initialized", 
             K(ret), K(bucket_id), K_(tmp_files_inited));
    return ret;
  }
  
  // 构建 IO 信息
  tmp_file::ObTmpFileIOInfo io_info;
  io_info.fd_ = bucket_fds_[bucket_id];
  io_info.dir_id_ = tmp_dir_id_;
  io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);  // 使用 DATA_READ（写入也适用）
  io_info.buf_ = reinterpret_cast<char*>(const_cast<FileRecord*>(records));
  io_info.size_ = count * sizeof(FileRecord);
  io_info.io_timeout_ms_ = 5000;
  
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S write_records_to_bucket: tmp file manager is null", K(ret));
    return ret;
  }
  
  if (OB_FAIL(tmp_mgr->write(MTL_ID(), io_info))) {
    LOG_WARN("BM25S write_records_to_bucket: fail to batch write to tmp file", 
             K(ret), K(bucket_id), K(count), K(io_info.size_));
  } else {
    ATOMIC_AAF(&bucket_write_offsets_[bucket_id], count * sizeof(FileRecord));
    LOG_DEBUG("BM25S write_records_to_bucket: batch write success", 
              K(bucket_id), K(count), K(io_info.size_));
  }
  
  return ret;
}

int ObFtsBM25SIndex::read_bucket(int64_t bucket_id, common::ObIArray<FileRecord> &records)
{
  int ret = OB_SUCCESS;
  
  if (bucket_id < 0 || bucket_id >= NUM_BUCKETS) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S read_bucket: invalid bucket id", K(ret), K(bucket_id), K(NUM_BUCKETS));
    return ret;
  }
  
  if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S read_bucket: tmp file not initialized", 
             K(ret), K(bucket_id), K_(tmp_files_inited));
    return ret;
  }
  
  tmp_file::ObTenantTmpFileManager *tmp_mgr = MTL(tmp_file::ObTenantTmpFileManager*);
  if (OB_ISNULL(tmp_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S read_bucket: tmp file manager is null", K(ret));
    return ret;
  }
  
  // 1. 获取文件大小
  int64_t file_size = 0;
  if (OB_FAIL(tmp_mgr->get_tmp_file_size(bucket_fds_[bucket_id], file_size))) {
    LOG_WARN("BM25S read_bucket: fail to get tmp file size", 
             K(ret), K(bucket_id), K(bucket_fds_[bucket_id]));
    return ret;
  }
  
  LOG_DEBUG("BM25S read_bucket: got file size", K(bucket_id), K(file_size));
  
  if (file_size == 0) {
    // 空桶，直接返回
    LOG_DEBUG("BM25S read_bucket: empty bucket", K(bucket_id));
    return OB_SUCCESS;
  }
  
  // 2. 检查文件大小是否合法
  if (file_size % sizeof(FileRecord) != 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S read_bucket: invalid tmp file size", 
             K(ret), K(bucket_id), K(file_size), K(sizeof(FileRecord)));
    return ret;
  }
  
  int64_t record_count = file_size / sizeof(FileRecord);
  LOG_INFO("BM25S read_bucket: reading bucket", 
           K(bucket_id), K(file_size), K(record_count));
  
  // 3. 分配读取缓冲区
  char *buf = static_cast<char*>(allocator_.alloc(file_size));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S read_bucket: fail to alloc buffer", K(ret), K(file_size));
    return ret;
  }
  
  // 4. 读取整个文件
  tmp_file::ObTmpFileIOInfo io_info;
  io_info.fd_ = bucket_fds_[bucket_id];
  io_info.dir_id_ = tmp_dir_id_;
  io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
  io_info.buf_ = buf;
  io_info.size_ = file_size;
  io_info.io_timeout_ms_ = 30000;  // 30 秒超时（大文件）
  
  tmp_file::ObTmpFileIOHandle handle;
  if (OB_FAIL(tmp_mgr->pread(MTL_ID(), io_info, 0 /*offset*/, handle))) {
    LOG_WARN("BM25S read_bucket: fail to read tmp file", 
             K(ret), K(bucket_id), K(file_size), K(bucket_fds_[bucket_id]));
    allocator_.free(buf);
    return ret;
  }
  
  // 5. 检查实际读取大小
  if (handle.get_done_size() != file_size) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("BM25S read_bucket: read size mismatch", 
             K(ret), K(handle.get_done_size()), K(file_size), K(bucket_id));
    allocator_.free(buf);
    return ret;
  }
  
  LOG_DEBUG("BM25S read_bucket: read completed", 
            K(bucket_id), K(file_size), K(handle.get_done_size()));
  
  // 6. 转换为记录数组
  FileRecord *record_array = reinterpret_cast<FileRecord*>(buf);
  if (OB_FAIL(records.reserve(records.count() + record_count))) {
    LOG_WARN("BM25S read_bucket: fail to reserve records", K(ret), K(record_count));
    allocator_.free(buf);
    return ret;
  }
  
  for (int64_t i = 0; OB_SUCC(ret) && i < record_count; ++i) {
    if (OB_FAIL(records.push_back(record_array[i]))) {
      LOG_WARN("BM25S read_bucket: fail to push record", K(ret), K(i));
    }
  }
  
  // 7. 释放缓冲区
  allocator_.free(buf);
  
  LOG_INFO("BM25S read_bucket: bucket read complete", K(bucket_id), K(record_count));
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
    
    // ========== 第二步：获取或分配 token_id（HashMap 读写锁）==========
    uint32_t token_id = 0;
    bool need_insert = false;
    
    // 先用读锁尝试查找
    {
      SpinRLockGuard rguard(token_map_lock_);
      int tmp_ret = collect_token_to_id_.get_refactored(token, token_id);
      if (OB_HASH_NOT_EXIST == tmp_ret) {
        need_insert = true;
      } else if (OB_SUCCESS != tmp_ret) {
        ret = tmp_ret;
        LOG_WARN("BM25S collect_raw_data: get token failed", K(ret), K(token));
      }
    }
    
    // 如果需要插入，用写锁
    if (OB_SUCC(ret) && need_insert) {
      SpinWLockGuard wguard(token_map_lock_);
      // 双重检查，可能其他线程已经插入
      int tmp_ret = collect_token_to_id_.get_refactored(token, token_id);
      if (OB_HASH_NOT_EXIST == tmp_ret) {
        // 确实不存在，分配新 ID
        token_id = static_cast<uint32_t>(collect_token_to_id_.size());
        
        // 复制 token 字符串（allocator 需要在锁内使用）
        char *token_buf = static_cast<char*>(allocator_.alloc(token.length()));
        if (OB_ISNULL(token_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("BM25S collect_raw_data: fail to alloc token buffer", K(ret), K(token.length()));
        } else {
          MEMCPY(token_buf, token.ptr(), token.length());
          common::ObString stored_token(token.length(), token_buf);
          if (OB_FAIL(collect_token_to_id_.set_refactored(stored_token, token_id, 0))) {
            LOG_WARN("BM25S collect_raw_data: fail to set token", K(ret), K(token));
          } else {
            LOG_DEBUG("BM25S collect_raw_data: new token registered", K(token_id), K(token));
          }
        }
      } else if (OB_SUCCESS == tmp_ret) {
        // 其他线程已经插入了，使用它的 token_id
      } else {
        ret = tmp_ret;
        LOG_WARN("BM25S collect_raw_data: get token failed in write lock", K(ret), K(token));
      }
    }
    
    // ========== 第三步：写入 TmpFile（桶锁保护）==========
    if (OB_SUCC(ret)) {
      int64_t bucket_id = token_id % NUM_BUCKETS;
      
      FileRecord record;
      record.token_id = token_id;
      record.doc_id = doc_id;
      record.tf = tf;
      record.doc_len = doc_len;
      
      // 使用桶锁保护写入（TmpFile 的 write 可能不是线程安全的）
      {
        common::ObSpinLockGuard bucket_guard(bucket_locks_[bucket_id]);
        if (OB_FAIL(write_to_bucket(bucket_id, record))) {
          LOG_WARN("BM25S collect_raw_data: fail to write to bucket", 
                   K(ret), K(bucket_id), K(token_id), K(doc_id));
          return ret;
        }
      }
      
      // 原子更新计数
      ATOMIC_AAF(&bucket_entry_counts_[bucket_id], 1);
      ATOMIC_AAF(&raw_entry_count_, 1);
      
      // 每写入 10 万条记录打印一次日志
      int64_t current_count = ATOMIC_LOAD(&raw_entry_count_);
      if (current_count % 100000 == 0) {
        LOG_INFO("BM25S collect_raw_data: progress", 
                 K(current_count), K(collect_token_to_id_.size()));
      }
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
    
    collect_token_to_id_.reuse();
    unique_docs_.reuse();
    total_doc_len_ = 0;
    raw_entry_count_ = 0;
    
    // 清理 CSC 索引
    scores_.reset();
    doc_ids_.reset();
    token_ptr_.reset();
    token_to_id_.reuse();
    
    total_docs_ = 0;
    avg_doc_len_ = 0.0;
    is_finalized_ = false;
    
    allocator_.reset();
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
    
    collect_token_to_id_.destroy();
    unique_docs_.destroy();
    token_to_id_.destroy();
    
    scores_.destroy();
    doc_ids_.destroy();
    token_ptr_.destroy();
    
    allocator_.reset();
    is_inited_ = false;
    is_finalized_ = false;
    LOG_INFO("BM25S destroy: destroy completed");
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

int ObFtsBM25SIndex::finalize()
{
  int ret = OB_SUCCESS;
  int64_t start_time = ObTimeUtility::current_time();
  
  LOG_INFO("BM25S finalize: starting finalize", K_(is_inited), K_(is_finalized), K_(tmp_files_inited));
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S finalize: not initialized", K(ret));
  } else if (is_finalized_) {
    LOG_INFO("BM25S finalize: already finalized");
  } else if (!tmp_files_inited_) {
    // 没有收集任何数据
    LOG_INFO("BM25S finalize: no data collected (tmp files not initialized)");
    SpinWLockGuard guard(lock_);
    is_finalized_ = true;
  } else {
    SpinWLockGuard guard(lock_);
    
    // 用于记录排序阶段内存信息
    int64_t max_bucket_size = 0;
    int64_t sort_mem_increase = 0;
    
    if (!is_finalized_) {
      // 计算统计信息
      total_docs_ = unique_docs_.size();
      avg_doc_len_ = (total_docs_ > 0) ? (double)total_doc_len_ / total_docs_ : 1.0;
      int64_t vocab_size = collect_token_to_id_.size();
      
      LOG_INFO("BM25S finalize: starting (TmpFile mode)",
               K_(total_docs), K_(avg_doc_len), K(vocab_size), K_(raw_entry_count), K(NUM_BUCKETS));
      
      // 第一遍：统计每个 token 的 df（使用 read_bucket）
      common::ObSEArray<int64_t, 1024> token_df;
      if (OB_FAIL(token_df.prepare_allocate(vocab_size))) {
        LOG_WARN("BM25S finalize: fail to allocate token_df", K(ret), K(vocab_size));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
        token_df.at(i) = 0;
      }
      
      int64_t total_df_count = 0;
      if (OB_SUCC(ret)) {
        LOG_INFO("BM25S finalize: phase 1 - counting df for each token");
        
        for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; bucket_id++) {
          // 使用 read_bucket 读取
          common::ObSEArray<FileRecord, 1024> bucket_records;
          if (OB_FAIL(read_bucket(bucket_id, bucket_records))) {
            LOG_WARN("BM25S finalize: fail to read bucket for df counting", K(ret), K(bucket_id));
            break;
          }
          
          // 统计 df
          for (int64_t i = 0; i < bucket_records.count(); i++) {
            const FileRecord &record = bucket_records.at(i);
            if (record.token_id < vocab_size) {
              token_df.at(record.token_id)++;
              total_df_count++;
            }
          }
          
          // 记录最大桶大小
          int64_t bucket_size = bucket_records.count() * sizeof(FileRecord);
          if (bucket_size > max_bucket_size) {
            max_bucket_size = bucket_size;
          }
          
          // 每处理 128 个桶打印一次进度
          if ((bucket_id + 1) % 128 == 0) {
            LOG_INFO("BM25S finalize: df counting progress", 
                     K(bucket_id), K(NUM_BUCKETS), K(total_df_count));
          }
        }
        LOG_INFO("BM25S finalize: phase 1 completed - df statistics", 
                 K(total_df_count), K_(raw_entry_count), K(vocab_size));
      }
      
      // 计算 token_ptr_（前缀和）
      if (OB_SUCC(ret)) {
        LOG_INFO("BM25S finalize: phase 2 - computing token_ptr (prefix sum)");
        
        if (OB_FAIL(token_ptr_.reserve(vocab_size + 1))) {
          LOG_WARN("BM25S finalize: fail to reserve token_ptr", K(ret));
        } else {
          token_ptr_.push_back(0);
          for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
            if (OB_FAIL(token_ptr_.push_back(token_ptr_.at(i) + token_df.at(i)))) {
              LOG_WARN("BM25S finalize: fail to push token_ptr", K(ret));
            }
          }
          LOG_INFO("BM25S finalize: phase 2 completed - token_ptr computed", 
                   K(vocab_size), "token_ptr_size", token_ptr_.count());
        }
      }
      
      // 预分配 CSC 数组
      if (OB_SUCC(ret)) {
        LOG_INFO("BM25S finalize: phase 3 - allocating CSC arrays", K_(raw_entry_count));
        
        if (OB_FAIL(scores_.prepare_allocate(raw_entry_count_))) {
          LOG_WARN("BM25S finalize: fail to allocate scores", K(ret), K_(raw_entry_count));
        } else if (OB_FAIL(doc_ids_.prepare_allocate(raw_entry_count_))) {
          LOG_WARN("BM25S finalize: fail to allocate doc_ids", K(ret), K_(raw_entry_count));
        } else {
          LOG_INFO("BM25S finalize: phase 3 completed - CSC arrays allocated");
        }
      }
      
      // 填充位置跟踪（每个 token 当前填充位置）
      common::ObSEArray<int64_t, 1024> token_fill_pos;
      if (OB_SUCC(ret)) {
        if (OB_FAIL(token_fill_pos.prepare_allocate(vocab_size))) {
          LOG_WARN("BM25S finalize: fail to allocate token_fill_pos", K(ret));
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
          token_fill_pos.at(i) = token_ptr_.at(i);
        }
      }
      
      // 复制 collect_token_to_id_ 到 token_to_id_
      if (OB_SUCC(ret)) {
        LOG_INFO("BM25S finalize: phase 4 - copying token_to_id map", K(vocab_size));
        
        for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
             OB_SUCC(ret) && it != collect_token_to_id_.end(); ++it) {
          if (OB_FAIL(token_to_id_.set_refactored(it->first, it->second, 0))) {
            LOG_WARN("BM25S finalize: fail to copy token_to_id", K(ret));
          }
        }
        LOG_INFO("BM25S finalize: phase 4 completed - token_to_id copied");
      }
      
      // 记录排序前内存
      int64_t mem_before_sort = 0, mem_after_sort = 0;
      FILE *status_file = fopen("/proc/self/status", "r");
      if (status_file) {
        char line[256];
        while (fgets(line, sizeof(line), status_file)) {
          if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &mem_before_sort);
            break;
          }
        }
        fclose(status_file);
      }
      LOG_INFO("BM25S finalize: memory before bucket processing", K(mem_before_sort));
      
      // 第二遍：逐桶处理，排序并填充 CSC 数组（使用 read_bucket）
      if (OB_SUCC(ret)) {
        LOG_INFO("BM25S finalize: phase 5 - processing buckets (sort and fill CSC)");
        
        int64_t processed_records = 0;
        for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; bucket_id++) {
          // 使用 read_bucket 读取
          common::ObSEArray<FileRecord, 1024> bucket_records;
          if (OB_FAIL(read_bucket(bucket_id, bucket_records))) {
            LOG_WARN("BM25S finalize: fail to read bucket for processing", K(ret), K(bucket_id));
            break;
          }
          
          int64_t bucket_count = bucket_records.count();
          if (bucket_count == 0) {
            continue;  // 空桶，跳过
          }
          
          // 桶内排序（使用指针直接排序）
          FileRecord *records_ptr = &bucket_records.at(0);
          std::sort(records_ptr, records_ptr + bucket_count);
          
          // 填充到 CSC 数组
          for (int64_t i = 0; OB_SUCC(ret) && i < bucket_count; i++) {
            const FileRecord &r = bucket_records.at(i);
            int64_t pos = token_fill_pos.at(r.token_id)++;
            
            double score = calc_bm25_score(r.tf, r.doc_len, token_df.at(r.token_id));
            scores_.at(pos) = static_cast<float>(score);
            doc_ids_.at(pos) = static_cast<uint32_t>(r.doc_id);
          }
          
          processed_records += bucket_count;
          
          // 每处理 64 个桶打印一次进度
          if ((bucket_id + 1) % 64 == 0) {
            LOG_INFO("BM25S finalize: bucket processing progress", 
                     K(bucket_id), K(NUM_BUCKETS), K(processed_records));
          }
        }
        LOG_INFO("BM25S finalize: phase 5 completed - all buckets processed", 
                 K(processed_records), K_(raw_entry_count));
      }
      
      // 记录排序后内存
      status_file = fopen("/proc/self/status", "r");
      if (status_file) {
        char line[256];
        while (fgets(line, sizeof(line), status_file)) {
          if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &mem_after_sort);
            break;
          }
        }
        fclose(status_file);
      }
      sort_mem_increase = mem_after_sort - mem_before_sort;
      LOG_INFO("BM25S finalize: memory after bucket processing", 
               K(mem_before_sort), K(mem_after_sort), K(sort_mem_increase), K(max_bucket_size));
      
      // 清理 TmpFile（删除所有桶文件）
      LOG_INFO("BM25S finalize: phase 6 - cleaning up TmpFile");
      cleanup_tmp_files();
      
      // 清理收集阶段的数据（但保留 allocator_，因为 token_to_id_ 中的 token 字符串需要它）
      collect_token_to_id_.reuse();
      unique_docs_.reuse();
      // 注意：不能调用 allocator_.reset()，因为 token_to_id_ 中的 ObString 指向这些内存！
      // allocator_.reset();
      
      if (OB_SUCC(ret)) {
        is_finalized_ = true;
        
        int64_t end_time = ObTimeUtility::current_time();
        double elapsed_ms = (end_time - start_time) / 1000.0;
        
        LOG_INFO("BM25S finalize: completed successfully (TmpFile mode)",
                 K_(total_docs), K_(avg_doc_len),
                 "vocab_size", token_to_id_.size(),
                 "total_entries", scores_.count(),
                 "elapsed_ms", elapsed_ms,
                 K(max_bucket_size),
                 K(sort_mem_increase));
      }
    }
  }
  
  return ret;
}

int ObFtsBM25SIndex::search(const ObIArray<ObString> &tokens,
                             int64_t id_limit,
                             int64_t top_k,
                             ObIArray<DocScore> &results)
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_ready())) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S index not ready", K(ret), K_(is_inited), K_(is_finalized));
  } else {
    SpinRLockGuard guard(lock_);
    
    // 使用哈希表累加分数（安全但稍慢）
    hash::ObHashMap<uint32_t, double, hash::NoPthreadDefendMode> doc_scores;
    if (OB_FAIL(doc_scores.create(1024, ObMemAttr(OB_SERVER_TENANT_ID, "BM25SSearch")))) {
      LOG_WARN("fail to create doc scores map", K(ret));
    }
    
    // 遍历每个查询 token
    for (int64_t t = 0; OB_SUCC(ret) && t < tokens.count(); t++) {
      const ObString &token = tokens.at(t);
      uint32_t token_id = 0;
      
      if (OB_SUCCESS != token_to_id_.get_refactored(token, token_id)) {
        continue;  // token 不存在，跳过
      }
      
      uint64_t start = token_ptr_.at(token_id);
      uint64_t end = token_ptr_.at(token_id + 1);
      
      // 二分剪枝：id < id_limit
      uint64_t valid_end = end;
      if (id_limit > 0) {
        int64_t lo = start, hi = end;
        while (lo < hi) {
          int64_t mid = (lo + hi) / 2;
          if (doc_ids_.at(mid) < id_limit) {
            lo = mid + 1;
          } else {
            hi = mid;
          }
        }
        valid_end = lo;
      }
      
      // 累加得分
      for (uint64_t i = start; OB_SUCC(ret) && i < valid_end; i++) {
        uint32_t doc_id = doc_ids_.at(i);
        float score = scores_.at(i);
        
        double existing = 0.0;
        int tmp_ret = doc_scores.get_refactored(doc_id, existing);
        if (OB_SUCCESS == tmp_ret) {
          if (OB_FAIL(doc_scores.set_refactored(doc_id, existing + score, 1))) {
            LOG_WARN("fail to update doc score", K(ret), K(doc_id));
          }
        } else if (OB_HASH_NOT_EXIST == tmp_ret) {
          if (OB_FAIL(doc_scores.set_refactored(doc_id, score, 0))) {
            LOG_WARN("fail to insert doc score", K(ret), K(doc_id));
          }
        } else {
          ret = tmp_ret;
          LOG_WARN("fail to get doc score", K(ret), K(doc_id));
        }
      }
    }
    
    // 收集所有结果
    if (OB_SUCC(ret)) {
      ObSEArray<DocScore, 256> all_results;
      for (hash::ObHashMap<uint32_t, double, hash::NoPthreadDefendMode>::iterator it = doc_scores.begin();
           OB_SUCC(ret) && it != doc_scores.end();
           ++it) {
        DocScore ds(it->first, it->second);
        if (OB_FAIL(all_results.push_back(ds))) {
          LOG_WARN("fail to push result", K(ret));
        }
      }
      
      // 部分排序取 top_k
      if (OB_SUCC(ret) && all_results.count() > 0) {
        int64_t k = std::min(top_k, (int64_t)all_results.count());
        
        if (all_results.count() > 1) {
          std::partial_sort(&all_results.at(0), 
                            &all_results.at(0) + k,
                            &all_results.at(0) + all_results.count(),
                            [](const DocScore &a, const DocScore &b) {
                              return a.score > b.score;
                            });
        }
        
        // 输出 top_k 结果
        for (int64_t i = 0; OB_SUCC(ret) && i < k; i++) {
          if (OB_FAIL(results.push_back(all_results.at(i)))) {
            LOG_WARN("fail to push result", K(ret));
          }
        }
      }
    }
    
    doc_scores.destroy();
  }
  
  return ret;
}

// ========== 栈上数组优化版本（无锁）==========
static const int64_t STACK_ARRAY_SIZE = 2000;  // 栈上预分配

int ObFtsBM25SIndex::search_with_array(const ObIArray<ObString> &tokens,
                                        int64_t id_limit,
                                        int64_t top_k,
                                        ObIArray<DocScore> &results)
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_ready())) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S index not ready", K(ret), K_(is_inited), K_(is_finalized));
  } else if (id_limit <= 0 || id_limit > STACK_ARRAY_SIZE) {
    // 超出栈数组范围，fallback 到哈希表版本
    ret = search(tokens, id_limit, top_k, results);
  } else {
    SpinRLockGuard guard(lock_);
    
    // 栈上分配数组（无锁，每次调用独立）
    double scores_array[STACK_ARRAY_SIZE] = {0};
    uint32_t touched_docs[STACK_ARRAY_SIZE];
    int64_t touched_count = 0;
    
    // 遍历每个查询 token
    for (int64_t t = 0; OB_SUCC(ret) && t < tokens.count(); t++) {
      const ObString &token = tokens.at(t);
      uint32_t token_id = 0;
      
      if (OB_SUCCESS != token_to_id_.get_refactored(token, token_id)) {
        continue;
      }
      
      uint64_t start = token_ptr_.at(token_id);
      uint64_t end = token_ptr_.at(token_id + 1);
      
      // 二分剪枝
      uint64_t valid_end = end;
      {
        int64_t lo = start, hi = end;
        while (lo < hi) {
          int64_t mid = (lo + hi) / 2;
          if (doc_ids_.at(mid) < id_limit) {
            lo = mid + 1;
          } else {
            hi = mid;
          }
        }
        valid_end = lo;
      }
      
      // 累加得分（数组直接访问，O(1)）
      for (uint64_t i = start; i < valid_end; i++) {
        uint32_t doc_id = doc_ids_.at(i);
        float score = scores_.at(i);
        
        if (scores_array[doc_id] == 0.0) {
          touched_docs[touched_count++] = doc_id;
        }
        scores_array[doc_id] += score;
      }
    }
    
    // 收集结果
    if (OB_SUCC(ret) && touched_count > 0) {
      ObSEArray<DocScore, 256> all_results;
      for (int64_t i = 0; OB_SUCC(ret) && i < touched_count; i++) {
        uint32_t doc_id = touched_docs[i];
        double score = scores_array[doc_id];
        if (score > 0.0) {
          if (OB_FAIL(all_results.push_back(DocScore(doc_id, score)))) {
            LOG_WARN("fail to push result", K(ret));
          }
        }
      }
      
      // 部分排序取 top_k
      if (OB_SUCC(ret) && all_results.count() > 0) {
        int64_t k = std::min(top_k, (int64_t)all_results.count());
        if (all_results.count() > 1) {
          std::partial_sort(&all_results.at(0), 
                            &all_results.at(0) + k,
                            &all_results.at(0) + all_results.count(),
                            [](const DocScore &a, const DocScore &b) {
                              return a.score > b.score;
                            });
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < k; i++) {
          if (OB_FAIL(results.push_back(all_results.at(i)))) {
            LOG_WARN("fail to push result", K(ret));
          }
        }
      }
    }
    // 栈上数组自动清理，无需手动清零
  }
  
  return ret;
}

bool ObFtsBM25SIndex::get_posting_list(const common::ObString &token, int64_t id_limit, uint64_t &start, uint64_t &end) const
{
  uint32_t token_id = 0;
  if (OB_SUCCESS != token_to_id_.get_refactored(token, token_id)) {
    return false;
  }
  start = token_ptr_.at(token_id);
  end = token_ptr_.at(token_id + 1);
  
  // 二分剪枝：找到 doc_id < id_limit 的范围
  if (id_limit > 0 && start < end) {
    int64_t lo = start, hi = end;
    while (lo < hi) {
      int64_t mid = (lo + hi) / 2;
      if (doc_ids_.at(mid) < id_limit) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    end = lo;
  }
  return start < end;  // 只有有效数据时返回 true
}

void ObFtsBM25SIndex::print_statistics() const
{
  SpinRLockGuard guard(lock_);
  
  LOG_INFO("ObFtsBM25SIndex statistics",
           K_(is_inited), K_(is_finalized),
           K_(total_docs), K_(avg_doc_len),
           "vocab_size", token_to_id_.size(),
           "total_entries", scores_.count(),
           "collect_vocab_size", collect_token_to_id_.size(),
           K_(raw_entry_count));
}

void ObFtsBM25SIndex::dump_csc_to_file(const char *filepath) const
{
  std::ofstream ofs(filepath);
  if (!ofs.is_open()) {
    LOG_INFO("fail to open file for CSC dump", K(filepath));
    return;
  }
  
  // 写入统计信息
  ofs << "# BM25S CSC Index Dump\n";
  ofs << "# total_docs: " << total_docs_ << "\n";
  ofs << "# avg_doc_len: " << avg_doc_len_ << "\n";
  ofs << "# vocab_size: " << token_to_id_.size() << "\n";
  ofs << "# total_entries: " << scores_.count() << "\n";
  ofs << "# k1: " << K1 << ", b: " << B << ", epsilon: " << EPSILON << "\n";
  ofs << "#\n";
  ofs << "# Format: token_id | token | start_idx | end_idx | entries...\n";
  ofs << "# Entry format: (doc_id, score)\n";
  ofs << "#\n\n";
  
  // 遍历所有 token
  for (TokenIdMap::const_iterator it = token_to_id_.begin(); 
       it != token_to_id_.end(); 
       ++it) {
    const ObString &token = it->first;
    uint32_t token_id = it->second;
    
    uint64_t start = token_ptr_.at(token_id);
    uint64_t end = token_ptr_.at(token_id + 1);
    
    // 写入 token 信息
    ofs << "TOKEN " << token_id << " | \"";
    ofs.write(token.ptr(), token.length());
    ofs << "\" | range=[" << start << ", " << end << ") | count=" << (end - start) << "\n";
    
    // 写入该 token 的所有 (doc_id, score) 对
    for (uint64_t i = start; i < end; i++) {
      ofs << "  (" << doc_ids_.at(i) << ", " << scores_.at(i) << ")\n";
    }
    ofs << "\n";
  }
  
  ofs.close();
  LOG_INFO("CSC index dumped to file", K(filepath), 
           "vocab_size", token_to_id_.size(),
           "total_entries", scores_.count());
}

void ObFtsBM25SIndex::dump_finalize_time(const char *filepath, double elapsed_ms,
                                          int64_t sort_file_size, int64_t sort_mem_increase) const
{
  std::ofstream ofs(filepath);
  if (!ofs.is_open()) {
    LOG_INFO("fail to open file for finalize time dump", K(filepath));
    return;
  }
  
  // 计算内存占用
  int64_t scores_mem = scores_.count() * sizeof(float);
  int64_t doc_ids_mem = doc_ids_.count() * sizeof(uint32_t);
  int64_t token_ptr_mem = token_ptr_.count() * sizeof(uint64_t);
  int64_t allocator_used = allocator_.used();
  int64_t allocator_total = allocator_.total();
  int64_t csc_total_mem = scores_mem + doc_ids_mem + token_ptr_mem;
  
  // 估算 HashMap 内存（粗略估计）
  int64_t hashmap_mem = token_to_id_.size() * (sizeof(ObString) + sizeof(uint32_t) + 32);  // 32 = bucket overhead
  
  int64_t total_mem = csc_total_mem + allocator_used + hashmap_mem;
  
  ofs << "# BM25S Finalize Time Report\n";
  ofs << "#\n";
  ofs << "# Statistics:\n";
  ofs << "#   total_docs: " << total_docs_ << "\n";
  ofs << "#   avg_doc_len: " << avg_doc_len_ << "\n";
  ofs << "#   vocab_size: " << token_to_id_.size() << "\n";
  ofs << "#   total_entries: " << scores_.count() << "\n";
  ofs << "#\n";
  ofs << "# Time:\n";
  ofs << "#   elapsed_ms: " << elapsed_ms << "\n";
  ofs << "#   elapsed_sec: " << (elapsed_ms / 1000.0) << "\n";
  ofs << "#\n";
  ofs << "# Performance:\n";
  if (scores_.count() > 0 && elapsed_ms > 0) {
    ofs << "#   entries_per_ms: " << (scores_.count() / elapsed_ms) << "\n";
    ofs << "#   us_per_entry: " << (elapsed_ms * 1000.0 / scores_.count()) << "\n";
  }
  ofs << "#\n";
  ofs << "# Memory Usage:\n";
  ofs << "#   scores_ array: " << scores_mem << " bytes (" << (scores_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   doc_ids_ array: " << doc_ids_mem << " bytes (" << (doc_ids_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   token_ptr_ array: " << token_ptr_mem << " bytes (" << (token_ptr_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   CSC total: " << csc_total_mem << " bytes (" << (csc_total_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   allocator used: " << allocator_used << " bytes (" << (allocator_used / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   allocator total: " << allocator_total << " bytes (" << (allocator_total / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   token_to_id_ hashmap (est): " << hashmap_mem << " bytes (" << (hashmap_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   Total (est): " << total_mem << " bytes (" << (total_mem / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#\n";
  ofs << "# Memory Efficiency:\n";
  if (scores_.count() > 0) {
    ofs << "#   bytes_per_entry: " << ((double)total_mem / scores_.count()) << "\n";
  }
  ofs << "#\n";
  ofs << "# Sorting Phase:\n";
  ofs << "#   raw_file_size: " << sort_file_size << " bytes (" << (sort_file_size / 1024.0 / 1024.0) << " MB)\n";
  ofs << "#   mem_increase_kb: " << sort_mem_increase << " KB (" << (sort_mem_increase / 1024.0) << " MB)\n";
  ofs << "#\n";
  
  ofs.close();
  LOG_INFO("Finalize time dumped to file", K(filepath), K(elapsed_ms),
           "total_mem_mb", (total_mem / 1024.0 / 1024.0));
}

} // end namespace sql
} // end namespace oceanbase
