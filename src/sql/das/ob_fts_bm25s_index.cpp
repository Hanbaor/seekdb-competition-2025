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
        
        // ========== Phase 2: 持久化到磁盘 ==========
        LOG_INFO("BM25S finalize: starting Phase 2 - persist to disk");
        const char *persist_dir = "/data/obcluster/store/bm25s_index";
        int persist_ret = persist_to_disk(persist_dir);
        if (OB_SUCCESS != persist_ret) {
          LOG_WARN("BM25S finalize: Phase 2 persist_to_disk FAILED (non-fatal)", 
                   K(persist_ret), K(persist_dir));
        } else {
          LOG_INFO("BM25S finalize: Phase 2 persist_to_disk SUCCESS", 
                   K(persist_dir), K_(index_file_path));
          
          // ========== Phase 3: 释放内存 CSC 结构（数据已落盘）==========
          // 保留 file_header_ 和 bucket_directory_ 用于 KVCache 查询
          // 释放大块内存：scores_, doc_ids_, token_ptr_
          LOG_INFO("BM25S finalize: Phase 3 - releasing in-memory CSC structure",
                   "scores_size", scores_.count(),
                   "doc_ids_size", doc_ids_.count(),
                   "token_ptr_size", token_ptr_.count());
          
          scores_.reset();
          doc_ids_.reset();
          token_ptr_.reset();
          // token_to_id_ 保留，用于快速检查 token 是否存在
          
          LOG_INFO("BM25S finalize: Phase 3 - in-memory CSC released, "
                   "now using file + KVCache for queries");
        }
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

int ObFtsBM25SIndex::persist_to_disk(const char *index_dir)
{
  int ret = OB_SUCCESS;
  
  if (OB_ISNULL(index_dir)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("BM25S persist_to_disk: index dir is null", K(ret));
    return ret;
  }
  
  if (!is_finalized_) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S persist_to_disk: index not finalized", K(ret));
    return ret;
  }
  
  LOG_INFO("BM25S persist_to_disk: starting", K(index_dir), K_(total_docs), 
           "vocab_size", token_to_id_.size(), "total_entries", scores_.count());
  
  int64_t start_time = ObTimeUtility::current_time();
  
  // 1. 创建目录
  LOG_INFO("BM25S persist_to_disk: step 1 - creating directory", K(index_dir));
  if (OB_FAIL(share::ObIODeviceLocalFileOp::mkdir(index_dir, 0755))) {
    if (OB_FILE_OR_DIRECTORY_EXIST == ret) {
      LOG_INFO("BM25S persist_to_disk: directory already exists, continuing", K(index_dir));
      ret = OB_SUCCESS;  // 目录已存在，继续
    } else {
      LOG_WARN("BM25S persist_to_disk: fail to mkdir", K(ret), K(index_dir));
      return ret;
    }
  } else {
    LOG_INFO("BM25S persist_to_disk: directory created successfully", K(index_dir));
  }
  
  // 2. 构建文件路径
  char file_path[512];
  int n = snprintf(file_path, sizeof(file_path), "%s/bm25s_index.dat", index_dir);
  if (n < 0 || n >= static_cast<int>(sizeof(file_path))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("BM25S persist_to_disk: file path too long", K(ret), K(index_dir));
    return ret;
  }
  LOG_INFO("BM25S persist_to_disk: step 2 - file path constructed", K(file_path));
  
  // 3. 打开文件
  LOG_INFO("BM25S persist_to_disk: step 3 - opening file for write", K(file_path));
  common::ObIOFd fd;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(file_path, 
                                          O_CREAT | O_WRONLY | O_TRUNC, 
                                          0644, fd))) {
    LOG_WARN("BM25S persist_to_disk: fail to open file", K(ret), K(file_path));
    return ret;
  }
  LOG_INFO("BM25S persist_to_disk: file opened successfully", 
           K(file_path), "fd.first_id", fd.first_id_, "fd.second_id", fd.second_id_);
  
  // 4. 写入各个 Section
  do {
    // 4.1 写入 Header 和 Bucket Directory（占位）
    LOG_INFO("BM25S persist_to_disk: step 4.1 - writing header and bucket directory");
    if (OB_FAIL(write_header_and_directory(fd))) {
      LOG_WARN("BM25S persist_to_disk: fail to write header", K(ret));
      break;
    }
    LOG_INFO("BM25S persist_to_disk: step 4.1 completed - header written");
    
    // 4.2 预计算 posting offsets
    LOG_INFO("BM25S persist_to_disk: step 4.2 - calculating posting offsets");
    ObArray<int64_t> posting_offsets;
    if (OB_FAIL(calc_posting_offsets(posting_offsets))) {
      LOG_WARN("BM25S persist_to_disk: fail to calc posting offsets", K(ret));
      break;
    }
    LOG_INFO("BM25S persist_to_disk: step 4.2 completed - posting offsets calculated", 
             "count", posting_offsets.count());
    
    // 4.3 写入 Vocab Buckets
    LOG_INFO("BM25S persist_to_disk: step 4.3 - writing vocab buckets");
    if (OB_FAIL(write_vocab_buckets(fd, posting_offsets))) {
      LOG_WARN("BM25S persist_to_disk: fail to write vocab buckets", K(ret));
      break;
    }
    LOG_INFO("BM25S persist_to_disk: step 4.3 completed - vocab buckets written",
             "vocab_buckets_size", file_header_.vocab_buckets_size);
    
    // 4.4 写入 Posting Data
    LOG_INFO("BM25S persist_to_disk: step 4.4 - writing posting data");
    if (OB_FAIL(write_posting_data(fd))) {
      LOG_WARN("BM25S persist_to_disk: fail to write posting data", K(ret));
      break;
    }
    LOG_INFO("BM25S persist_to_disk: step 4.4 completed - posting data written",
             "posting_data_size", file_header_.posting_data_size);
    
    // 4.5 更新 Header
    LOG_INFO("BM25S persist_to_disk: step 4.5 - updating header with final offsets");
    if (OB_FAIL(update_header(fd))) {
      LOG_WARN("BM25S persist_to_disk: fail to update header", K(ret));
      break;
    }
    LOG_INFO("BM25S persist_to_disk: step 4.5 completed - header updated");
  } while (0);
  
  // 5. 同步并关闭
  if (fd.is_normal_file()) {
    share::ObIODeviceLocalFileOp::fsync(fd);
    share::ObIODeviceLocalFileOp::close(fd);
  }
  
  if (OB_SUCC(ret)) {
    MEMCPY(index_file_path_, file_path, strlen(file_path) + 1);
    
    int64_t end_time = ObTimeUtility::current_time();
    double elapsed_ms = (end_time - start_time) / 1000.0;
    
    LOG_INFO("BM25S persist_to_disk: completed", K(file_path), K_(total_docs), 
             "vocab_size", token_to_id_.size(), 
             "total_entries", scores_.count(),
             "elapsed_ms", elapsed_ms,
             "posting_data_size", file_header_.posting_data_size);
  }
  
  return ret;
}

int ObFtsBM25SIndex::write_header_and_directory(const common::ObIOFd &fd)
{
  int ret = OB_SUCCESS;
  int64_t write_size = 0;
  
  // 1. 准备 Header
  file_header_.reset();
  file_header_.magic = BM25SIndexFileHeader::MAGIC;
  file_header_.version = BM25SIndexFileHeader::CURRENT_VERSION;
  file_header_.total_docs = total_docs_;
  file_header_.avg_doc_len = avg_doc_len_;
  file_header_.vocab_size = token_to_id_.size();
  file_header_.total_postings = scores_.count();
  file_header_.bucket_num = NUM_PERSIST_BUCKETS;
  file_header_.create_timestamp = ObTimeUtility::current_time();
  
  // 计算各 Section 偏移
  file_header_.bucket_dir_offset = BM25SIndexFileHeader::HEADER_SIZE;
  file_header_.bucket_dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  file_header_.vocab_buckets_offset = file_header_.bucket_dir_offset + file_header_.bucket_dir_size;
  // vocab_buckets_size 和 posting_data_offset 后面计算
  
  // 2. 写入 Header
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, &file_header_, 
                                                  sizeof(file_header_), 0, write_size))) {
    LOG_WARN("BM25S write_header_and_directory: fail to write header", K(ret));
    return ret;
  }
  
  // 3. 准备 Bucket Directory（先写入空的，后面更新）
  int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  char *dir_buf = static_cast<char*>(allocator_.alloc(dir_size));
  if (OB_ISNULL(dir_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S write_header_and_directory: fail to alloc dir_buf", K(ret), K(dir_size));
    return ret;
  }
  memset(dir_buf, 0, dir_size);
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, dir_buf, dir_size,
                                                  file_header_.bucket_dir_offset, write_size))) {
    LOG_WARN("BM25S write_header_and_directory: fail to write bucket directory", K(ret));
    allocator_.free(dir_buf);
    return ret;
  }
  
  allocator_.free(dir_buf);
  
  LOG_INFO("BM25S write_header_and_directory: completed", 
           K(file_header_.bucket_dir_offset), K(dir_size));
  
  return ret;
}

int ObFtsBM25SIndex::calc_posting_offsets(common::ObIArray<int64_t> &posting_offsets)
{
  int ret = OB_SUCCESS;
  
  int64_t vocab_size = token_ptr_.count() - 1;
  if (vocab_size <= 0) {
    LOG_INFO("BM25S calc_posting_offsets: empty vocab");
    return ret;
  }
  
  if (OB_FAIL(posting_offsets.reserve(vocab_size))) {
    LOG_WARN("BM25S calc_posting_offsets: fail to reserve", K(ret), K(vocab_size));
    return ret;
  }
  
  int64_t current_offset = 0;
  for (int64_t token_id = 0; OB_SUCC(ret) && token_id < vocab_size; ++token_id) {
    if (OB_FAIL(posting_offsets.push_back(current_offset))) {
      LOG_WARN("BM25S calc_posting_offsets: fail to push", K(ret), K(token_id));
      break;
    }
    
    uint64_t start = token_ptr_.at(token_id);
    uint64_t end = token_ptr_.at(token_id + 1);
    int64_t count = end - start;
    
    // 每个 posting entry = doc_id(4) + score(4) = 8 bytes
    current_offset += count * (sizeof(uint32_t) + sizeof(float));
  }
  
  file_header_.posting_data_size = current_offset;
  
  LOG_INFO("BM25S calc_posting_offsets: completed", 
           K(vocab_size), K(current_offset));
  
  return ret;
}

int ObFtsBM25SIndex::write_vocab_buckets(const common::ObIOFd &fd,
                                          const common::ObIArray<int64_t> &posting_offsets)
{
  int ret = OB_SUCCESS;
  
  const int32_t bucket_num = file_header_.bucket_num;
  
  // 1. 分配 Bucket Directory 数组
  bucket_directory_ = static_cast<BucketDirectoryEntry*>(
      allocator_.alloc(bucket_num * sizeof(BucketDirectoryEntry)));
  if (OB_ISNULL(bucket_directory_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S write_vocab_buckets: fail to alloc bucket_directory", K(ret));
    return ret;
  }
  for (int32_t i = 0; i < bucket_num; ++i) {
    bucket_directory_[i].reset();
  }
  
  // 2. 将所有 token 按桶分组
  ObArray<ObArray<VocabEntryMem>> bucket_entries;
  if (OB_FAIL(bucket_entries.prepare_allocate(bucket_num))) {
    LOG_WARN("BM25S write_vocab_buckets: fail to alloc bucket_entries", K(ret));
    return ret;
  }
  
  // 遍历 token_to_id_，分配到各个桶
  for (TokenIdMap::const_iterator it = token_to_id_.begin();
       OB_SUCC(ret) && it != token_to_id_.end(); ++it) {
    const ObString &token = it->first;
    uint32_t token_id = it->second;
    
    VocabEntryMem entry;
    entry.token_hash = token.hash();
    entry.token = token;
    entry.token_id = token_id;
    
    // 从 token_ptr_ 获取 posting 信息
    if (token_id < static_cast<uint32_t>(token_ptr_.count() - 1)) {
      uint64_t start = token_ptr_.at(token_id);
      uint64_t end = token_ptr_.at(token_id + 1);
      entry.posting_count = static_cast<int32_t>(end - start);
      // posting_offset 已预计算
      if (token_id < static_cast<uint32_t>(posting_offsets.count())) {
        entry.posting_offset = posting_offsets.at(token_id);
      } else {
        entry.posting_offset = 0;
      }
    } else {
      entry.posting_count = 0;
      entry.posting_offset = 0;
    }
    
    int32_t bucket_id = entry.token_hash % bucket_num;
    if (OB_FAIL(bucket_entries.at(bucket_id).push_back(entry))) {
      LOG_WARN("BM25S write_vocab_buckets: fail to push entry", K(ret), K(bucket_id));
      break;
    }
  }
  
  if (OB_FAIL(ret)) {
    return ret;
  }
  
  // 3. 对每个桶内的 entry 按 hash 排序
  for (int32_t i = 0; i < bucket_num; ++i) {
    ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
    if (entries.count() > 1) {
      std::sort(&entries.at(0), &entries.at(0) + entries.count(),
          [](const VocabEntryMem &a, const VocabEntryMem &b) {
              return a.token_hash < b.token_hash;
          });
    }
  }
  
  // 4. 计算每个桶的大小和偏移
  int64_t current_offset = 0;
  for (int32_t i = 0; i < bucket_num; ++i) {
    ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
    bucket_directory_[i].offset = current_offset;
    bucket_directory_[i].entry_count = entries.count();
    
    // 计算桶大小
    int64_t bucket_size = 0;
    for (int64_t j = 0; j < entries.count(); ++j) {
      // 固定部分 32 字节 + token 字符串
      bucket_size += 32 + entries.at(j).token.length();
    }
    bucket_directory_[i].size = static_cast<int32_t>(bucket_size);
    current_offset += bucket_size;
  }
  
  file_header_.vocab_buckets_size = current_offset;
  file_header_.posting_data_offset = file_header_.vocab_buckets_offset + current_offset;
  
  // 5. 写入每个桶的数据
  int64_t write_offset = file_header_.vocab_buckets_offset;
  int64_t total_written = 0;
  for (int32_t i = 0; OB_SUCC(ret) && i < bucket_num; ++i) {
    ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
    if (entries.empty()) {
      continue;
    }
    
    // 分配桶缓冲区
    int64_t bucket_size = bucket_directory_[i].size;
    char *buf = static_cast<char*>(allocator_.alloc(bucket_size));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("BM25S write_vocab_buckets: fail to alloc bucket buf", K(ret), K(i), K(bucket_size));
      break;
    }
    
    // 填充桶数据
    char *p = buf;
    for (int64_t j = 0; j < entries.count(); ++j) {
      VocabEntryMem &entry = entries.at(j);
      
      // token_hash (8 bytes)
      memcpy(p, &entry.token_hash, 8); p += 8;
      // token_len (4 bytes)
      int32_t token_len = entry.token.length();
      memcpy(p, &token_len, 4); p += 4;
      // token_data (变长)
      memcpy(p, entry.token.ptr(), token_len); p += token_len;
      // token_id (4 bytes)
      memcpy(p, &entry.token_id, 4); p += 4;
      // posting_offset (8 bytes)
      memcpy(p, &entry.posting_offset, 8); p += 8;
      // posting_count (4 bytes)
      memcpy(p, &entry.posting_count, 4); p += 4;
      // reserved (4 bytes)
      int32_t reserved = 0;
      memcpy(p, &reserved, 4); p += 4;
    }
    
    // 写入桶
    int64_t write_size = 0;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, buf, bucket_size,
                                                    write_offset, write_size))) {
      LOG_WARN("BM25S write_vocab_buckets: fail to write bucket", K(ret), K(i));
      allocator_.free(buf);
      break;
    }
    
    write_offset += bucket_size;
    total_written += bucket_size;
    allocator_.free(buf);
    
    // 每处理 512 个桶打印一次进度
    if ((i + 1) % 512 == 0) {
      LOG_INFO("BM25S write_vocab_buckets: progress", K(i), K(bucket_num), K(total_written));
    }
  }
  
  LOG_INFO("BM25S write_vocab_buckets: completed", 
           K(bucket_num), K(total_written), K(file_header_.vocab_buckets_size));
  
  return ret;
}

int ObFtsBM25SIndex::write_posting_data(const common::ObIOFd &fd)
{
  int ret = OB_SUCCESS;
  
  if (scores_.empty()) {
    LOG_INFO("BM25S write_posting_data: empty scores, skipping");
    return ret;
  }
  
  int64_t posting_offset = file_header_.posting_data_offset;
  int64_t total_written = 0;
  
  // 按 token_id 顺序写入每个 token 的 posting list
  int64_t vocab_size = token_ptr_.count() - 1;
  
  // 使用批量写入优化
  const int64_t BATCH_SIZE = 1024 * 1024;  // 1MB 批次
  char *batch_buf = static_cast<char*>(allocator_.alloc(BATCH_SIZE));
  if (OB_ISNULL(batch_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S write_posting_data: fail to alloc batch_buf", K(ret));
    return ret;
  }
  
  int64_t batch_used = 0;
  int64_t batch_start_offset = posting_offset;
  
  for (int64_t token_id = 0; OB_SUCC(ret) && token_id < vocab_size; ++token_id) {
    uint64_t start = token_ptr_.at(token_id);
    uint64_t end = token_ptr_.at(token_id + 1);
    int64_t count = end - start;
    
    if (count == 0) {
      continue;
    }
    
    // 计算数据大小
    int64_t doc_ids_size = count * sizeof(uint32_t);
    int64_t scores_size = count * sizeof(float);
    int64_t total_size = doc_ids_size + scores_size;
    
    // 检查是否需要刷新批次
    if (batch_used + total_size > BATCH_SIZE) {
      // 写入当前批次
      if (batch_used > 0) {
        int64_t write_size = 0;
        if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, batch_buf, batch_used,
                                                        batch_start_offset, write_size))) {
          LOG_WARN("BM25S write_posting_data: fail to write batch", K(ret));
          break;
        }
        total_written += batch_used;
      }
      batch_start_offset = posting_offset + total_written;
      batch_used = 0;
    }
    
    // 如果单个 token 的数据超过批次大小，直接写入
    if (total_size > BATCH_SIZE) {
      char *temp_buf = static_cast<char*>(allocator_.alloc(total_size));
      if (OB_ISNULL(temp_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("BM25S write_posting_data: fail to alloc temp_buf", K(ret), K(total_size));
        break;
      }
      
      // 复制 doc_ids
      memcpy(temp_buf, &doc_ids_.at(start), doc_ids_size);
      // 复制 scores
      memcpy(temp_buf + doc_ids_size, &scores_.at(start), scores_size);
      
      int64_t write_size = 0;
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, temp_buf, total_size,
                                                      batch_start_offset, write_size))) {
        LOG_WARN("BM25S write_posting_data: fail to write large posting", K(ret), K(token_id));
        allocator_.free(temp_buf);
        break;
      }
      
      total_written += total_size;
      batch_start_offset = posting_offset + total_written;
      allocator_.free(temp_buf);
    } else {
      // 添加到批次缓冲区
      memcpy(batch_buf + batch_used, &doc_ids_.at(start), doc_ids_size);
      memcpy(batch_buf + batch_used + doc_ids_size, &scores_.at(start), scores_size);
      batch_used += total_size;
    }
    
    // 每处理 10000 个 token 打印一次进度
    if ((token_id + 1) % 10000 == 0) {
      LOG_INFO("BM25S write_posting_data: progress", K(token_id), K(vocab_size), K(total_written));
    }
  }
  
  // 写入最后一个批次
  if (OB_SUCC(ret) && batch_used > 0) {
    int64_t write_size = 0;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, batch_buf, batch_used,
                                                    batch_start_offset, write_size))) {
      LOG_WARN("BM25S write_posting_data: fail to write final batch", K(ret));
    } else {
      total_written += batch_used;
    }
  }
  
  allocator_.free(batch_buf);
  
  LOG_INFO("BM25S write_posting_data: completed", 
           K(vocab_size), K(total_written), K(file_header_.posting_data_size));
  
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
  
  LOG_INFO("BM25S finalize_and_persist: starting", K(index_path));
  int64_t start_time = ObTimeUtility::current_time();
  
  // 0. 更新统计信息
  total_docs_ = unique_docs_.size();
  avg_doc_len_ = (total_docs_ > 0) ? 
                 static_cast<double>(total_doc_len_) / total_docs_ : 1.0;
  int64_t vocab_size = collect_token_to_id_.size();
  
  LOG_INFO("BM25S finalize_and_persist: statistics", 
           K_(total_docs), K_(avg_doc_len), K(vocab_size), K_(raw_entry_count));
  
  // 1. 创建持久化文件
  common::ObIOFd fd;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(index_path, 
                                          O_CREAT | O_WRONLY | O_TRUNC, 
                                          0644, fd))) {
    LOG_WARN("BM25S finalize_and_persist: fail to open file", K(ret), K(index_path));
    return ret;
  }
  
  // 2. 初始化元数据数组
  ObArray<int64_t> token_df;           // 每个 token 的文档频率
  ObArray<int64_t> posting_offsets;    // 每个 token 的 posting 偏移
  ObArray<int64_t> token_written_count; // 每个 token 已写入的记录数
  
  if (OB_FAIL(token_df.prepare_allocate(vocab_size)) ||
      OB_FAIL(posting_offsets.prepare_allocate(vocab_size)) ||
      OB_FAIL(token_written_count.prepare_allocate(vocab_size))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S finalize_and_persist: fail to alloc metadata arrays", K(ret), K(vocab_size));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  for (int64_t i = 0; i < vocab_size; ++i) {
    token_df.at(i) = 0;
    posting_offsets.at(i) = 0;
    token_written_count.at(i) = 0;
  }
  
  // 3. 第一次遍历所有桶：统计 df
  LOG_INFO("BM25S finalize_and_persist: phase 1 - counting df");
  int64_t total_records = 0;
  for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
    ObArray<FileRecord> records;
    if (OB_FAIL(read_bucket(bucket_id, records))) {
      LOG_WARN("BM25S finalize_and_persist: fail to read bucket", K(ret), K(bucket_id));
      break;
    }
    if (records.empty()) {
      continue;
    }
    
    // 统计每个 token 的 df
    for (int64_t i = 0; i < records.count(); ++i) {
      const FileRecord &rec = records.at(i);
      if (rec.token_id < static_cast<uint32_t>(vocab_size)) {
        token_df.at(rec.token_id)++;
        total_records++;
      }
    }
    
    // 每处理 128 个桶打印一次进度
    if ((bucket_id + 1) % 128 == 0) {
      LOG_INFO("BM25S finalize_and_persist: df counting progress", 
               K(bucket_id), K(NUM_BUCKETS), K(total_records));
    }
  }
  
  if (OB_FAIL(ret)) {
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  LOG_INFO("BM25S finalize_and_persist: phase 1 completed", K(total_records));
  
  // 4. 计算 posting_offsets 和 posting_data_size
  int64_t current_offset = 0;
  for (int64_t token_id = 0; token_id < vocab_size; ++token_id) {
    posting_offsets.at(token_id) = current_offset;
    // 每个 posting = doc_id(4) + score(4) = 8 bytes
    current_offset += token_df.at(token_id) * 8;
  }
  int64_t posting_data_size = current_offset;
  
  // 5. 计算 vocab_buckets_size
  int64_t vocab_buckets_size = 0;
  for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
       it != collect_token_to_id_.end(); ++it) {
    // 每个 entry = 32 bytes 固定部分 + token_len
    vocab_buckets_size += 32 + it->first.length();
  }
  
  // 6. 初始化文件头
  file_header_.reset();
  file_header_.magic = BM25SIndexFileHeader::MAGIC;
  file_header_.version = BM25SIndexFileHeader::CURRENT_VERSION;
  file_header_.total_docs = total_docs_;
  file_header_.avg_doc_len = avg_doc_len_;
  file_header_.vocab_size = vocab_size;
  file_header_.total_postings = total_records;
  file_header_.bucket_num = NUM_PERSIST_BUCKETS;
  file_header_.create_timestamp = ObTimeUtility::current_time();
  
  file_header_.bucket_dir_offset = BM25SIndexFileHeader::HEADER_SIZE;
  file_header_.bucket_dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  file_header_.vocab_buckets_offset = file_header_.bucket_dir_offset + file_header_.bucket_dir_size;
  file_header_.vocab_buckets_size = vocab_buckets_size;
  file_header_.posting_data_offset = file_header_.vocab_buckets_offset + vocab_buckets_size;
  file_header_.posting_data_size = posting_data_size;
  
  // 7. 写入 Header 和 Bucket Directory（占位）
  LOG_INFO("BM25S finalize_and_persist: phase 2 - writing header");
  if (OB_FAIL(write_header_and_directory(fd))) {
    LOG_WARN("BM25S finalize_and_persist: fail to write header", K(ret));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  // 8. 写入 Vocab Buckets
  LOG_INFO("BM25S finalize_and_persist: phase 3 - writing vocab buckets");
  if (OB_FAIL(write_vocab_buckets_streaming(fd, posting_offsets, token_df))) {
    LOG_WARN("BM25S finalize_and_persist: fail to write vocab buckets", K(ret));
    share::ObIODeviceLocalFileOp::close(fd);
    return ret;
  }
  
  // 9. 第二次遍历所有桶：计算 BM25 并写入 Posting Data
  LOG_INFO("BM25S finalize_and_persist: phase 4 - writing posting data");
  int64_t written_postings = 0;
  for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
    ObArray<FileRecord> records;
    if (OB_FAIL(read_bucket(bucket_id, records))) {
      LOG_WARN("BM25S finalize_and_persist: fail to read bucket for posting", K(ret), K(bucket_id));
      break;
    }
    if (records.empty()) {
      continue;
    }
    
    // 排序
    std::sort(&records.at(0), &records.at(0) + records.count());
    
    // 遍历记录，计算 BM25 并写入文件
    for (int64_t i = 0; OB_SUCC(ret) && i < records.count(); ++i) {
      const FileRecord &rec = records.at(i);
      uint32_t token_id = rec.token_id;
      
      if (token_id >= static_cast<uint32_t>(vocab_size)) {
        continue;
      }
      
      // 获取这个 token 的信息
      int64_t df = token_df.at(token_id);
      int64_t token_base = file_header_.posting_data_offset + 
                           posting_offsets.at(token_id);
      int64_t idx = token_written_count.at(token_id);
      
      // 计算 BM25 分数
      float score = calc_bm25_score_f(rec.tf, rec.doc_len, df);
      uint32_t doc_id = static_cast<uint32_t>(rec.doc_id);
      
      // 计算写入位置
      // doc_ids 区域：[token_base, token_base + df * 4)
      // scores 区域：[token_base + df * 4, token_base + df * 8)
      int64_t doc_id_offset = token_base + idx * sizeof(uint32_t);
      int64_t score_offset = token_base + df * sizeof(uint32_t) + idx * sizeof(float);
      
      // 写入 doc_id
      int64_t write_size = 0;
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
              fd.second_id_, &doc_id, sizeof(doc_id), 
              doc_id_offset, write_size))) {
        LOG_WARN("BM25S finalize_and_persist: fail to write doc_id", K(ret), K(token_id), K(idx));
        break;
      }
      
      // 写入 score
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
              fd.second_id_, &score, sizeof(score),
              score_offset, write_size))) {
        LOG_WARN("BM25S finalize_and_persist: fail to write score", K(ret), K(token_id), K(idx));
        break;
      }
      
      token_written_count.at(token_id)++;
      written_postings++;
    }
    
    // 每处理 64 个桶打印一次进度
    if ((bucket_id + 1) % 64 == 0) {
      LOG_INFO("BM25S finalize_and_persist: posting write progress", 
               K(bucket_id), K(NUM_BUCKETS), K(written_postings));
    }
  }
  
  // 10. 更新 Header
  if (OB_SUCC(ret)) {
    LOG_INFO("BM25S finalize_and_persist: phase 5 - updating header");
    ret = update_header(fd);
  }
  
  // 11. 同步并关闭文件
  share::ObIODeviceLocalFileOp::fsync(fd);
  share::ObIODeviceLocalFileOp::close(fd);
  
  // 12. 清理 TmpFile
  cleanup_tmp_files();
  
  // 13. 复制 token_to_id（用于查询）
  if (OB_SUCC(ret)) {
    for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
         OB_SUCC(ret) && it != collect_token_to_id_.end(); ++it) {
      if (OB_FAIL(token_to_id_.set_refactored(it->first, it->second, 0))) {
        LOG_WARN("BM25S finalize_and_persist: fail to copy token_to_id", K(ret));
      }
    }
  }
  
  // 14. 清理收集阶段数据
  collect_token_to_id_.reuse();
  unique_docs_.reuse();
  
  if (OB_SUCC(ret)) {
    is_finalized_ = true;
    MEMCPY(index_file_path_, index_path, strlen(index_path) + 1);
    
    int64_t end_time = ObTimeUtility::current_time();
    double elapsed_ms = (end_time - start_time) / 1000.0;
    
    LOG_INFO("BM25S finalize_and_persist: completed successfully", 
             K(index_path), K_(total_docs), K(vocab_size), 
             K(written_postings), K(posting_data_size),
             "elapsed_ms", elapsed_ms);
  }
  
  return ret;
}

int ObFtsBM25SIndex::write_vocab_buckets_streaming(
    const common::ObIOFd &fd,
    const common::ObIArray<int64_t> &posting_offsets,
    const common::ObIArray<int64_t> &token_df)
{
  int ret = OB_SUCCESS;
  const int32_t bucket_num = NUM_PERSIST_BUCKETS;
  
  // 1. 分配 Bucket Directory
  bucket_directory_ = static_cast<BucketDirectoryEntry*>(
      allocator_.alloc(bucket_num * sizeof(BucketDirectoryEntry)));
  if (OB_ISNULL(bucket_directory_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("BM25S write_vocab_buckets_streaming: fail to alloc bucket_directory", K(ret));
    return ret;
  }
  for (int32_t i = 0; i < bucket_num; ++i) {
    bucket_directory_[i].reset();
  }
  
  // 2. 将 token 按 hash 分桶
  ObArray<ObArray<VocabEntryMem>> bucket_entries;
  if (OB_FAIL(bucket_entries.prepare_allocate(bucket_num))) {
    LOG_WARN("BM25S write_vocab_buckets_streaming: fail to alloc bucket_entries", K(ret));
    return ret;
  }
  
  for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
       OB_SUCC(ret) && it != collect_token_to_id_.end(); ++it) {
    VocabEntryMem entry;
    entry.token_hash = it->first.hash();
    entry.token = it->first;
    entry.token_id = it->second;
    
    if (it->second < static_cast<uint32_t>(posting_offsets.count())) {
      entry.posting_offset = posting_offsets.at(it->second);
      entry.posting_count = static_cast<int32_t>(token_df.at(it->second));
    } else {
      entry.posting_offset = 0;
      entry.posting_count = 0;
    }
    
    int32_t bucket_id = entry.token_hash % bucket_num;
    if (OB_FAIL(bucket_entries.at(bucket_id).push_back(entry))) {
      LOG_WARN("BM25S write_vocab_buckets_streaming: fail to push entry", K(ret), K(bucket_id));
      break;
    }
  }
  
  if (OB_FAIL(ret)) {
    return ret;
  }
  
  // 3. 对每个桶按 hash 排序并写入
  int64_t current_offset = 0;
  int64_t write_offset = file_header_.vocab_buckets_offset;
  int64_t total_written = 0;
  
  for (int32_t i = 0; OB_SUCC(ret) && i < bucket_num; ++i) {
    ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
    
    bucket_directory_[i].offset = current_offset;
    bucket_directory_[i].entry_count = entries.count();
    
    if (entries.empty()) {
      bucket_directory_[i].size = 0;
      continue;
    }
    
    // 排序
    std::sort(&entries.at(0), &entries.at(0) + entries.count(),
        [](const VocabEntryMem &a, const VocabEntryMem &b) {
            return a.token_hash < b.token_hash;
        });
    
    // 计算桶大小
    int64_t bucket_size = 0;
    for (int64_t j = 0; j < entries.count(); ++j) {
      bucket_size += 32 + entries.at(j).token.length();
    }
    bucket_directory_[i].size = static_cast<int32_t>(bucket_size);
    
    // 分配缓冲区并填充
    char *buf = static_cast<char*>(allocator_.alloc(bucket_size));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("BM25S write_vocab_buckets_streaming: fail to alloc buf", K(ret), K(bucket_size));
      break;
    }
    
    char *p = buf;
    for (int64_t j = 0; j < entries.count(); ++j) {
      VocabEntryMem &e = entries.at(j);
      memcpy(p, &e.token_hash, 8); p += 8;
      int32_t token_len = e.token.length();
      memcpy(p, &token_len, 4); p += 4;
      memcpy(p, e.token.ptr(), token_len); p += token_len;
      memcpy(p, &e.token_id, 4); p += 4;
      memcpy(p, &e.posting_offset, 8); p += 8;
      memcpy(p, &e.posting_count, 4); p += 4;
      int32_t reserved = 0;
      memcpy(p, &reserved, 4); p += 4;
    }
    
    // 写入文件
    int64_t write_size = 0;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
            fd.second_id_, buf, bucket_size, write_offset, write_size))) {
      LOG_WARN("BM25S write_vocab_buckets_streaming: fail to write bucket", K(ret), K(i));
      allocator_.free(buf);
      break;
    }
    
    write_offset += bucket_size;
    current_offset += bucket_size;
    total_written += bucket_size;
    allocator_.free(buf);
    
    // 每处理 512 个桶打印一次进度
    if ((i + 1) % 512 == 0) {
      LOG_INFO("BM25S write_vocab_buckets_streaming: progress", K(i), K(bucket_num), K(total_written));
    }
  }
  
  LOG_INFO("BM25S write_vocab_buckets_streaming: completed", 
           K(bucket_num), K(total_written));
  
  return ret;
}

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
  
  // 6. 加载 Bucket Directory
  int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
  bucket_directory_ = static_cast<BucketDirectoryEntry*>(allocator_.alloc(dir_size));
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
  
  // 7. 加载 Vocab Buckets 并重建 token_to_id_
  // 注意：init() 函数在收集阶段创建了一个小的 token_to_id_（用于构建索引）
  // 查询阶段需要从磁盘加载更大的 token_to_id_（大小为 vocab_size * 2）
  // 所以需要先销毁旧的，再创建新的
  if (token_to_id_.created()) {
    token_to_id_.destroy();
  }
  if (OB_FAIL(token_to_id_.create(file_header_.vocab_size * 2, 
                                   ObMemAttr(OB_SERVER_TENANT_ID, "BM25STokenMap")))) {
    LOG_WARN("BM25S load_from_disk: fail to create token_to_id", K(ret));
    close_index_file();
    return ret;
  }
  
  // 同时构建 token_ptr_
  if (OB_FAIL(token_ptr_.reserve(file_header_.vocab_size + 1))) {
    LOG_WARN("BM25S load_from_disk: fail to reserve token_ptr", K(ret));
    close_index_file();
    return ret;
  }
  
  // 临时数组存储 (token_id, posting_offset, posting_count)
  struct TokenInfo {
    uint32_t token_id;
    int64_t posting_offset;
    int32_t posting_count;
    
    TO_STRING_KV(K(token_id), K(posting_offset), K(posting_count));
  };
  ObArray<TokenInfo> token_infos;
  if (OB_FAIL(token_infos.prepare_allocate(file_header_.vocab_size))) {
    LOG_WARN("BM25S load_from_disk: fail to alloc token_infos", K(ret));
    close_index_file();
    return ret;
  }
  
  // 遍历所有 bucket，读取 vocab entries
  for (int32_t bucket_id = 0; OB_SUCC(ret) && bucket_id < file_header_.bucket_num; ++bucket_id) {
    BucketDirectoryEntry &dir = bucket_directory_[bucket_id];
    if (dir.entry_count == 0 || dir.size == 0) {
      continue;
    }
    
    // 读取桶数据
    char *buf = static_cast<char*>(allocator_.alloc(dir.size));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("BM25S load_from_disk: fail to alloc bucket buf", K(ret), K(bucket_id), K(dir.size));
      break;
    }
    
    int64_t bucket_offset = file_header_.vocab_buckets_offset + dir.offset;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, buf,
                                                   dir.size, bucket_offset, read_size))) {
      LOG_WARN("BM25S load_from_disk: fail to read bucket", K(ret), K(bucket_id));
      allocator_.free(buf);
      break;
    }
    
    // 解析桶数据
    char *p = buf;
    char *end = buf + dir.size;
    for (int32_t i = 0; OB_SUCC(ret) && i < dir.entry_count && p < end; ++i) {
      // token_hash (8 bytes) - 跳过
      p += 8;
      // token_len (4 bytes)
      int32_t token_len = 0;
      memcpy(&token_len, p, 4); p += 4;
      // token_data (变长)
      char *token_copy = static_cast<char*>(allocator_.alloc(token_len));
      if (OB_ISNULL(token_copy)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      memcpy(token_copy, p, token_len); p += token_len;
      ObString token(token_len, token_copy);
      
      // token_id (4 bytes)
      uint32_t token_id = 0;
      memcpy(&token_id, p, 4); p += 4;
      // posting_offset (8 bytes)
      int64_t posting_offset = 0;
      memcpy(&posting_offset, p, 8); p += 8;
      // posting_count (4 bytes)
      int32_t posting_count = 0;
      memcpy(&posting_count, p, 4); p += 4;
      // reserved (4 bytes) - 跳过
      p += 4;
      
      // 添加到 token_to_id_
      if (OB_FAIL(token_to_id_.set_refactored(token, token_id, 0))) {
        LOG_WARN("BM25S load_from_disk: fail to set token_to_id", K(ret), K(token_id));
        break;
      }
      
      // 记录 token info
      if (token_id < static_cast<uint32_t>(file_header_.vocab_size)) {
        token_infos.at(token_id).token_id = token_id;
        token_infos.at(token_id).posting_offset = posting_offset;
        token_infos.at(token_id).posting_count = posting_count;
      }
    }
    
    allocator_.free(buf);
  }
  
  // 构建 token_ptr_
  if (OB_SUCC(ret)) {
    token_ptr_.push_back(0);
    for (int64_t i = 0; OB_SUCC(ret) && i < file_header_.vocab_size; ++i) {
      int64_t count = token_infos.at(i).posting_count;
      if (OB_FAIL(token_ptr_.push_back(token_ptr_.at(i) + count))) {
        LOG_WARN("BM25S load_from_disk: fail to push token_ptr", K(ret));
      }
    }
  }
  
  // 8. 加载 Posting Data
  if (OB_SUCC(ret)) {
    int64_t total_postings = file_header_.total_postings;
    if (OB_FAIL(scores_.prepare_allocate(total_postings))) {
      LOG_WARN("BM25S load_from_disk: fail to alloc scores", K(ret), K(total_postings));
      close_index_file();
      return ret;
    }
    if (OB_FAIL(doc_ids_.prepare_allocate(total_postings))) {
      LOG_WARN("BM25S load_from_disk: fail to alloc doc_ids", K(ret), K(total_postings));
      close_index_file();
      return ret;
    }
    
    // 按 token 顺序读取 posting data
    for (int64_t token_id = 0; OB_SUCC(ret) && token_id < file_header_.vocab_size; ++token_id) {
      int64_t posting_offset = token_infos.at(token_id).posting_offset;
      int32_t posting_count = token_infos.at(token_id).posting_count;
      
      if (posting_count == 0) {
        continue;
      }
      
      int64_t file_offset = file_header_.posting_data_offset + posting_offset;
      int64_t idx_start = token_ptr_.at(token_id);
      
      // 读取 doc_ids
      int64_t doc_ids_size = posting_count * sizeof(uint32_t);
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_, 
                                                     &doc_ids_.at(idx_start),
                                                     doc_ids_size, file_offset, read_size))) {
        LOG_WARN("BM25S load_from_disk: fail to read doc_ids", K(ret), K(token_id));
        break;
      }
      
      // 读取 scores
      int64_t scores_offset = file_offset + posting_count * sizeof(uint32_t);
      int64_t scores_size = posting_count * sizeof(float);
      if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(persist_fd_.second_id_,
                                                     &scores_.at(idx_start),
                                                     scores_size, scores_offset, read_size))) {
        LOG_WARN("BM25S load_from_disk: fail to read scores", K(ret), K(token_id));
        break;
      }
      
      // 每处理 10000 个 token 打印一次进度
      if ((token_id + 1) % 10000 == 0) {
        LOG_INFO("BM25S load_from_disk: loading progress", 
                 K(token_id), K(file_header_.vocab_size));
      }
    }
  }
  
  // 9. 恢复统计信息
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
  
  // 如果索引文件已打开，直接返回
  if (persist_file_opened_) {
    LOG_DEBUG("BM25S ensure_ready: index file already opened");
    return OB_SUCCESS;
  }
  
  // 如果内存 CSC 索引可用（未释放），也可以使用
  if (is_finalized_ && scores_.count() > 0) {
    LOG_DEBUG("BM25S ensure_ready: in-memory CSC available");
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
    allocator_.free(bucket_directory_);
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
// Phase 3: KVCache 查询实现
// ========================================================================

int ObFtsBM25SIndex::find_token_in_file(const common::ObString &token,
                                         int64_t &posting_offset,
                                         int64_t &posting_count)
{
  int ret = OB_SUCCESS;
  
  if (!persist_file_opened_) {
    ret = OB_NOT_INIT;
    LOG_WARN("BM25S find_token_in_file: index file not opened", K(ret));
    return ret;
  }
  
  // 计算 token 的 hash
  uint64_t token_hash = common::murmurhash(token.ptr(), token.length(), 0);
  
  // 1. 计算桶 ID（与 Phase 2 写入时一致）
  int32_t bucket_id = static_cast<int32_t>(token_hash % file_header_.bucket_num);
  BucketDirectoryEntry &bucket = bucket_directory_[bucket_id];
  
  if (bucket.entry_count == 0) {
    LOG_DEBUG("BM25S find_token_in_file: bucket is empty", 
              K(token), K(bucket_id), K(token_hash));
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
  
  // 3. 遍历桶内 Entry 查找 token（按 Phase 2 格式解析）
  ret = OB_ENTRY_NOT_EXIST;
  char *p = bucket_data;
  char *end = bucket_data + bucket.size;
  
  while (p < end) {
    // 解析 VocabEntry（与 Phase 2 写入顺序一致）
    uint64_t entry_hash = *reinterpret_cast<uint64_t*>(p); p += 8;
    int32_t entry_token_len = *reinterpret_cast<int32_t*>(p); p += 4;
    const char *entry_token_data = p; p += entry_token_len;
    uint32_t entry_token_id = *reinterpret_cast<uint32_t*>(p); p += 4;  // 不使用
    (void)entry_token_id;
    int64_t entry_posting_offset = *reinterpret_cast<int64_t*>(p); p += 8;
    int32_t entry_posting_count = *reinterpret_cast<int32_t*>(p); p += 4;
    p += 4;  // reserved
    
    // 比较 token（先比 hash，再比内容）
    if (entry_hash == token_hash &&
        entry_token_len == token.length() &&
        0 == MEMCMP(entry_token_data, token.ptr(), token.length())) {
      // 找到了！
      posting_offset = entry_posting_offset;
      posting_count = entry_posting_count;
      ret = OB_SUCCESS;
      
      LOG_DEBUG("BM25S find_token_in_file: token found", 
                K(token), K(bucket_id), K(posting_offset), K(posting_count));
      break;
    }
    
    // 优化：桶内按 hash 排序，如果 entry_hash > token_hash 可以提前退出
    if (entry_hash > token_hash) {
      break;
    }
  }
  
  cache_allocator_.free(bucket_data);
  return ret;
}

int ObFtsBM25SIndex::load_posting_to_cache(const common::ObString &token,
                                            const ObBM25SPostingValue *&posting,
                                            common::ObKVCacheHandle &handle)
{
  int ret = OB_SUCCESS;
  
  LOG_DEBUG("BM25S load_posting_to_cache: loading from file", K(token));
  
  // 1. 从文件查找 token 的 posting 信息
  int64_t posting_offset = 0;
  int64_t posting_count = 0;
  
  if (OB_FAIL(find_token_in_file(token, posting_offset, posting_count))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("BM25S load_posting_to_cache: fail to find token in file", K(ret), K(token));
    }
    return ret;
  }
  
  if (posting_count == 0) {
    ret = OB_ENTRY_NOT_EXIST;
    LOG_DEBUG("BM25S load_posting_to_cache: token has no postings", K(token));
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
  if (OB_FAIL(ret)) {
    LOG_WARN("BM25S load_posting_to_cache: fail to put posting to cache", K(ret), K(token));
  } else {
    LOG_DEBUG("BM25S load_posting_to_cache: posting cached successfully", 
              K(token), K(posting_count));
  }
  
  // 7. 释放临时缓冲区（数据已被 deep_copy 到 KVCache）
  cache_allocator_.free(buf);
  
  return ret;
}

int ObFtsBM25SIndex::get_posting_from_cache(const common::ObString &token,
                                             const ObBM25SPostingValue *&posting,
                                             common::ObKVCacheHandle &handle)
{
  int ret = OB_SUCCESS;
  
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
    // 缓存命中
    LOG_DEBUG("BM25S get_posting_from_cache: cache HIT", K(token), K(posting->count()));
    return OB_SUCCESS;
  }
  
  if (OB_ENTRY_NOT_EXIST == ret) {
    // 2. 缓存未命中，从文件加载
    LOG_DEBUG("BM25S get_posting_from_cache: cache MISS, loading from file", K(token));
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
