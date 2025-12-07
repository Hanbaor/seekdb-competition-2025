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

// ========== 持久化日志函数 ==========
static const char* BM25S_LOG_FILE = "/data/obcluster/bm25s_debug.log";

void bm25s_log(const char* fmt, ...) {
  FILE* f = fopen(BM25S_LOG_FILE, "a");
  if (f) {
    // 时间戳
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // 线程ID
    pthread_t tid = pthread_self();
    
    fprintf(f, "[%s][tid=%lu] ", time_buf, (unsigned long)tid);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
  }
}

// 静态成员定义
const int64_t ObFtsBM25SIndex::NUM_BUCKETS;

ObFtsBM25SIndex::ObFtsBM25SIndex()
  : is_inited_(false),
    is_finalized_(false),
    active_collectors_(0),
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
  bucket_dir_path_[0] = '\0';
  for (int i = 0; i < NUM_BUCKETS; i++) {
    bucket_files_[i] = nullptr;
    bucket_entry_counts_[i] = 0;
  }
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
  } else {
    SpinWLockGuard guard(lock_);
    if (!is_inited_) {
      // 创建分桶目录
      snprintf(bucket_dir_path_, sizeof(bucket_dir_path_), "./bm25s_buckets_%d", getpid());
      if (mkdir(bucket_dir_path_, 0755) != 0 && errno != EEXIST) {
        ret = OB_IO_ERROR;
        LOG_WARN("fail to create bucket directory", K(ret), K(bucket_dir_path_));
      }
      
      // 创建分桶文件
      if (OB_SUCC(ret)) {
        for (int i = 0; OB_SUCC(ret) && i < NUM_BUCKETS; i++) {
          char bucket_path[512];
          snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, i);
          bucket_files_[i] = fopen(bucket_path, "wb+");
          if (OB_ISNULL(bucket_files_[i])) {
            ret = OB_IO_ERROR;
            LOG_WARN("fail to create bucket file", K(ret), K(i), K(bucket_path));
          }
          bucket_entry_counts_[i] = 0;
        }
      }
      
      if (OB_FAIL(ret)) {
        // 清理已创建的文件
        for (int i = 0; i < NUM_BUCKETS; i++) {
          if (bucket_files_[i] != nullptr) {
            fclose(bucket_files_[i]);
            bucket_files_[i] = nullptr;
          }
        }
      } else if (OB_FAIL(collect_token_to_id_.create(DEFAULT_BUCKET_COUNT,
                                    ObMemAttr(OB_SERVER_TENANT_ID, "BM25SCollectMap")))) {
        LOG_WARN("fail to create collect token map", K(ret));
      } else if (OB_FAIL(unique_docs_.create(DEFAULT_BUCKET_COUNT,
                                              ObMemAttr(OB_SERVER_TENANT_ID, "BM25SDocSet")))) {
        LOG_WARN("fail to create unique docs set", K(ret));
      } else if (OB_FAIL(token_to_id_.create(DEFAULT_BUCKET_COUNT,
                                              ObMemAttr(OB_SERVER_TENANT_ID, "BM25STokenMap")))) {
        LOG_WARN("fail to create token to id map", K(ret));
      } else {
        raw_entry_count_ = 0;
        is_inited_ = true;
        LOG_INFO("ObFtsBM25SIndex initialized (bucketed)", K(bucket_dir_path_), K(NUM_BUCKETS));
        bm25s_log("INIT: bucket_dir=%s, num_buckets=%ld", bucket_dir_path_, NUM_BUCKETS);
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

int ObFtsBM25SIndex::collect_raw_data(const common::ObString &token, int64_t doc_id, int32_t tf, int32_t doc_len)
{
  int ret = OB_SUCCESS;
  
  if (OB_FAIL(ensure_inited())) {
    // 初始化失败
  } else if (token.empty()) {
    // 空 token，跳过
  } else if (is_finalized_) {
    // 已经 finalize，跳过（无需加锁检查，因为 finalize 后不会再变）
  } else {
    // ========== 第一步：获取或分配 token_id（HashMap 读写锁）==========
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
        } else {
          MEMCPY(token_buf, token.ptr(), token.length());
          common::ObString stored_token(token.length(), token_buf);
          if (OB_FAIL(collect_token_to_id_.set_refactored(stored_token, token_id, 0))) {
            // set 失败
          }
        }
      } else if (OB_SUCCESS == tmp_ret) {
        // 其他线程已经插入了，使用它的 token_id
      } else {
        ret = tmp_ret;
      }
    }
    
    // ========== 第二步：写入桶文件（桶锁）==========
    if (OB_SUCC(ret)) {
      int bucket_id = token_id % NUM_BUCKETS;
      
      FileRecord record;
      record.token_id = token_id;
      record.doc_id = doc_id;
      record.tf = tf;
      record.doc_len = doc_len;
      
      // 只锁对应的桶
      {
        common::ObSpinLockGuard bucket_guard(bucket_locks_[bucket_id]);
        FILE *bucket_file = bucket_files_[bucket_id];
        
        if (OB_ISNULL(bucket_file)) {
          ret = OB_NOT_INIT;
        } else if (fwrite(&record, sizeof(FileRecord), 1, bucket_file) != 1) {
          ret = OB_IO_ERROR;
        }
      }
      
      if (OB_SUCC(ret)) {
        // 原子更新计数
        ATOMIC_AAF(&bucket_entry_counts_[bucket_id], 1);
        int64_t cur_count = ATOMIC_AAF(&raw_entry_count_, 1);
        
        // 每 1000000 条记录输出一次日志（减少 IO 开销）
        if (cur_count % 1000000 == 0) {
          bm25s_log("COLLECT: raw_entry_count=%ld, unique_docs=%ld, vocab_size=%ld, active_collectors=%ld",
                    cur_count, unique_docs_.size(), collect_token_to_id_.size(), 
                    ATOMIC_LOAD(&active_collectors_));
        }
      }
    }
    
    // ========== 第三步：统计唯一文档（DocSet 读写锁）==========
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
          }
        }
      }
    }
  }
  
  return ret;
}

void ObFtsBM25SIndex::reset()
{
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    // 关闭并删除分桶文件
    for (int i = 0; i < NUM_BUCKETS; i++) {
      if (OB_NOT_NULL(bucket_files_[i])) {
        fclose(bucket_files_[i]);
        bucket_files_[i] = nullptr;
      }
      bucket_entry_counts_[i] = 0;
    }
    // 删除桶文件和目录
    if (bucket_dir_path_[0] != '\0') {
      char bucket_path[512];
      for (int i = 0; i < NUM_BUCKETS; i++) {
        snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, i);
        unlink(bucket_path);
      }
      rmdir(bucket_dir_path_);
      bucket_dir_path_[0] = '\0';
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
    LOG_INFO("ObFtsBM25SIndex reset");
  }
}

void ObFtsBM25SIndex::destroy()
{
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    // 关闭并删除分桶文件
    for (int i = 0; i < NUM_BUCKETS; i++) {
      if (OB_NOT_NULL(bucket_files_[i])) {
        fclose(bucket_files_[i]);
        bucket_files_[i] = nullptr;
      }
    }
    if (bucket_dir_path_[0] != '\0') {
      char bucket_path[512];
      for (int i = 0; i < NUM_BUCKETS; i++) {
        snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, i);
        unlink(bucket_path);
      }
      rmdir(bucket_dir_path_);
      bucket_dir_path_[0] = '\0';
    }
    
    collect_token_to_id_.destroy();
    unique_docs_.destroy();
    token_to_id_.destroy();
    
    scores_.destroy();
    doc_ids_.destroy();
    token_ptr_.destroy();
    
    allocator_.reset();
    is_inited_ = false;
    is_finalized_ = false;
    LOG_INFO("ObFtsBM25SIndex destroyed");
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
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not initialized", K(ret));
  } else if (is_finalized_) {
    LOG_INFO("ObFtsBM25SIndex already finalized");
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
      
      LOG_INFO("BM25S finalize starting (bucketed)",
               K_(total_docs), K_(avg_doc_len), K(vocab_size), K_(raw_entry_count), K(NUM_BUCKETS));
      bm25s_log("FINALIZE_START: total_docs=%ld, avg_doc_len=%.2f, vocab_size=%ld, raw_entry_count=%ld, active_collectors=%ld",
                total_docs_, avg_doc_len_, vocab_size, raw_entry_count_, ATOMIC_LOAD(&active_collectors_));
      
      // 关闭所有桶文件准备读取
      for (int i = 0; i < NUM_BUCKETS; i++) {
        if (bucket_files_[i] != nullptr) {
          fflush(bucket_files_[i]);
          fclose(bucket_files_[i]);
          bucket_files_[i] = nullptr;
        }
      }
      
      // 第一遍：统计每个 token 的 df
      common::ObSEArray<int64_t, 1024> token_df;
      if (OB_FAIL(token_df.prepare_allocate(vocab_size))) {
        LOG_WARN("fail to allocate token_df", K(ret), K(vocab_size));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
        token_df.at(i) = 0;
      }
      
      int64_t total_df_count = 0;
      if (OB_SUCC(ret)) {
        for (int bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; bucket_id++) {
          char bucket_path[512];
          snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, bucket_id);
          FILE *f = fopen(bucket_path, "rb");
          if (f == nullptr) {
            continue;  // 空桶
          }
          FileRecord record;
          while (fread(&record, sizeof(FileRecord), 1, f) == 1) {
            if (record.token_id < vocab_size) {
              token_df.at(record.token_id)++;
              total_df_count++;
            }
          }
          fclose(f);
        }
        LOG_INFO("BM25S df statistics", K(total_df_count), K_(raw_entry_count), K(vocab_size));
      }
      
      // 计算 token_ptr_（前缀和）
      if (OB_SUCC(ret)) {
        if (OB_FAIL(token_ptr_.reserve(vocab_size + 1))) {
          LOG_WARN("fail to reserve token_ptr", K(ret));
        } else {
          token_ptr_.push_back(0);
          for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
            if (OB_FAIL(token_ptr_.push_back(token_ptr_.at(i) + token_df.at(i)))) {
              LOG_WARN("fail to push token_ptr", K(ret));
            }
          }
        }
      }
      
      // 预分配 CSC 数组
      if (OB_SUCC(ret)) {
        if (OB_FAIL(scores_.prepare_allocate(raw_entry_count_))) {
          LOG_WARN("fail to allocate scores", K(ret), K_(raw_entry_count));
        } else if (OB_FAIL(doc_ids_.prepare_allocate(raw_entry_count_))) {
          LOG_WARN("fail to allocate doc_ids", K(ret), K_(raw_entry_count));
        }
      }
      
      // 填充位置跟踪（每个 token 当前填充位置）
      common::ObSEArray<int64_t, 1024> token_fill_pos;
      if (OB_SUCC(ret)) {
        if (OB_FAIL(token_fill_pos.prepare_allocate(vocab_size))) {
          LOG_WARN("fail to allocate token_fill_pos", K(ret));
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < vocab_size; i++) {
          token_fill_pos.at(i) = token_ptr_.at(i);
        }
      }
      
      // 复制 collect_token_to_id_ 到 token_to_id_
      if (OB_SUCC(ret)) {
        for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
             OB_SUCC(ret) && it != collect_token_to_id_.end(); ++it) {
          if (OB_FAIL(token_to_id_.set_refactored(it->first, it->second, 0))) {
            LOG_WARN("fail to copy token_to_id", K(ret));
          }
        }
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
      
      // 第二遍：逐桶处理，排序并填充 CSC 数组
      if (OB_SUCC(ret)) {
        for (int bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; bucket_id++) {
          char bucket_path[512];
          snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, bucket_id);
          
          // 获取文件大小来确定记录数
          struct stat st;
          if (stat(bucket_path, &st) != 0 || st.st_size == 0) {
            continue;  // 文件不存在或为空
          }
          
          int64_t bucket_count = st.st_size / sizeof(FileRecord);
          int64_t bucket_size = st.st_size;
          
          // 跟踪最大桶大小
          if (bucket_size > max_bucket_size) {
            max_bucket_size = bucket_size;
          }
          
          // 读取桶文件到内存
          FileRecord *records = static_cast<FileRecord*>(malloc(bucket_size));
          if (OB_ISNULL(records)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("fail to allocate bucket buffer", K(ret), K(bucket_id), K(bucket_size));
          } else {
            FILE *f = fopen(bucket_path, "rb");
            if (f == nullptr) {
              ret = OB_IO_ERROR;
              LOG_WARN("fail to open bucket file", K(ret), K(bucket_path));
            } else {
              size_t read_count = fread(records, sizeof(FileRecord), bucket_count, f);
              fclose(f);
              
              if (read_count != (size_t)bucket_count) {
                ret = OB_IO_ERROR;
                LOG_WARN("fail to read bucket file", K(ret), K(read_count), K(bucket_count));
              } else {
                // 桶内排序
                std::sort(records, records + bucket_count);
                
                // 填充到 CSC 数组
                for (int64_t i = 0; OB_SUCC(ret) && i < bucket_count; i++) {
                  const FileRecord &r = records[i];
                  int64_t pos = token_fill_pos.at(r.token_id)++;
                  
                  double score = calc_bm25_score(r.tf, r.doc_len, token_df.at(r.token_id));
                  scores_.at(pos) = static_cast<float>(score);
                  doc_ids_.at(pos) = static_cast<uint32_t>(r.doc_id);
                }
              }
            }
            free(records);
          }
          
          if ((bucket_id + 1) % 32 == 0) {
            LOG_INFO("BM25S bucket progress", K(bucket_id), K(NUM_BUCKETS));
          }
        }
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
      LOG_INFO("BM25S bucketed processing done", K(mem_before_sort), K(mem_after_sort), K(sort_mem_increase), K(max_bucket_size));
      
      // 删除桶文件
      if (bucket_dir_path_[0] != '\0') {
        char bucket_path[512];
        for (int i = 0; i < NUM_BUCKETS; i++) {
          snprintf(bucket_path, sizeof(bucket_path), "%s/bucket_%03d.bin", bucket_dir_path_, i);
          unlink(bucket_path);
        }
        rmdir(bucket_dir_path_);
      }
      
      // 清理收集阶段的数据
      collect_token_to_id_.reuse();
      unique_docs_.reuse();
      allocator_.reset();
      
      if (OB_SUCC(ret)) {
        is_finalized_ = true;
        
        int64_t end_time = ObTimeUtility::current_time();
        double elapsed_ms = (end_time - start_time) / 1000.0;
        
        LOG_INFO("ObFtsBM25SIndex finalized (bucketed)",
                 K_(total_docs), K_(avg_doc_len),
                 "vocab_size", token_to_id_.size(),
                 "total_entries", scores_.count(),
                 "elapsed_ms", elapsed_ms,
                 K(max_bucket_size));
        bm25s_log("FINALIZE_DONE: total_docs=%ld, vocab_size=%ld, total_entries=%ld, elapsed_ms=%.2f",
                  total_docs_, token_to_id_.size(), scores_.count(), elapsed_ms);
        
        dump_csc_to_file("./bm25s_csc_index.txt");
        dump_finalize_time("./bm25s_finalize_time.txt", elapsed_ms, max_bucket_size, sort_mem_increase);
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
    
    // 累加每个文档的得分
    hash::ObHashMap<uint32_t, double, hash::NoPthreadDefendMode> doc_scores;
    if (OB_FAIL(doc_scores.create(1024, ObMemAttr(OB_SERVER_TENANT_ID, "BM25SSearch")))) {
      LOG_WARN("fail to create doc scores map", K(ret));
    }
    
    // 遍历每个查询 token
    for (int64_t t = 0; OB_SUCC(ret) && t < tokens.count(); t++) {
      const ObString &token = tokens.at(t);
      uint32_t token_id = 0;
      
      if (token_to_id_.get_refactored(token, token_id) != OB_SUCCESS) {
        // token 不存在，跳过
        continue;
      }
      
      uint64_t start = token_ptr_.at(token_id);
      uint64_t end = token_ptr_.at(token_id + 1);
      
      // 二分剪枝：id < id_limit
      uint64_t valid_end = end;
      if (id_limit > 0) {
        // 使用二分查找找到第一个 >= id_limit 的位置
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
        
        // 使用指针进行排序，因为 ObSEArray 迭代器不兼容 std::sort
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
