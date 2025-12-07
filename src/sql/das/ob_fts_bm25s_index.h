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

#ifndef OCEANBASE_SQL_DAS_OB_FTS_BM25S_INDEX_H_
#define OCEANBASE_SQL_DAS_OB_FTS_BM25S_INDEX_H_

#include "lib/hash/ob_hashmap.h"
#include "lib/hash/ob_hashset.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/lock/ob_spin_rwlock.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_print_utils.h"
#include <fstream>
#include <cstdio>

namespace oceanbase
{
namespace sql
{

// 持久化日志函数（定义在 ob_fts_bm25s_index.cpp）
void bm25s_log(const char* fmt, ...);

/**
 * @brief BM25S 预计算索引
 * 
 * 两阶段构建：
 * 1. 收集阶段：DDL 时收集 (token, doc_id, tf, doc_len) 到 HashMap
 * 2. Finalize 阶段：DDL 完成后计算 BM25 得分，构建 CSC 格式
 * 
 * CSC 格式：
 * - scores_[]: 预计算的 BM25 得分
 * - doc_ids_[]: 文档 ID（每个 token 内有序）
 * - token_ptr_[]: 每个 token 的起止位置
 */
class ObFtsBM25SIndex
{
public:
  // ========== 收集阶段的原始数据 ==========
  struct RawEntry
  {
    int64_t doc_id;
    int32_t tf;        // 词频
    int32_t doc_len;   // 文档长度
    
    RawEntry() : doc_id(0), tf(0), doc_len(0) {}
    RawEntry(int64_t d, int32_t t, int32_t l) : doc_id(d), tf(t), doc_len(l) {}
    
    TO_STRING_KV(K(doc_id), K(tf), K(doc_len));
  };
  
  // ========== 查询结果 ==========
  struct DocScore
  {
    int64_t doc_id;
    double score;
    
    DocScore() : doc_id(0), score(0.0) {}
    DocScore(int64_t d, double s) : doc_id(d), score(s) {}
    
    bool operator>(const DocScore &other) const { return score > other.score; }
    
    TO_STRING_KV(K(doc_id), K(score));
  };

  // ========== 文件记录（用于流式写入）==========
  struct FileRecord
  {
    uint32_t token_id;
    int64_t doc_id;
    int32_t tf;
    int32_t doc_len;
    
    bool operator<(const FileRecord &other) const {
      return token_id < other.token_id || 
             (token_id == other.token_id && doc_id < other.doc_id);
    }
  } __attribute__((packed));  // 20 字节
  typedef common::hash::ObHashMap<common::ObString, uint32_t,
                                   common::hash::NoPthreadDefendMode> TokenIdMap;
  typedef common::hash::ObHashSet<int64_t, common::hash::NoPthreadDefendMode> DocIdSet;

  static const int64_t DEFAULT_BUCKET_COUNT = 100000;
  static const int64_t NUM_BUCKETS = 128;  // 分桶数量，用于减少排序内存峰值
  
  // BM25 参数（与 ObExprBM25 保持一致）
  static constexpr double K1 = 1.2;
  static constexpr double B = 0.75;
  static constexpr double EPSILON = 0.25;

  static ObFtsBM25SIndex &get_instance();

  int init();
  void reset();
  void destroy();

  // ========== 收集阶段接口（文件流式）==========
  /**
   * @brief 收集单个 token 的数据（写入临时文件）
   * @param token token 字符串
   * @param doc_id 文档 ID
   * @param tf 词频
   * @param doc_len 文档长度
   */
  int collect_raw_data(const common::ObString &token, int64_t doc_id, int32_t tf, int32_t doc_len);
  
  /**
   * @brief 批量收集一个文档的所有 token（写入临时文件）
   * @param doc_id 文档 ID
   * @param doc_len 文档长度
   * @param ft_word_map 分词结果 Map<token, count>
   */
  template <typename FTWordMapType>
  int collect_document(int64_t doc_id, int32_t doc_len, const FTWordMapType &ft_word_map);

  // ========== Finalize 阶段接口 ==========
  /**
   * @brief 计算 BM25 得分并构建 CSC 索引（DDL 完成时调用）
   */
  int finalize();

  // ========== 查询接口 ==========
  /**
   * @brief 搜索查询（已分词的 tokens）
   * @param tokens 查询词列表
   * @param id_limit id < id_limit 过滤条件（0 表示不过滤）
   * @param top_k 返回 top_k 个结果
   * @param results 输出结果（doc_id, score）
   */
  int search(const common::ObIArray<common::ObString> &tokens,
             int64_t id_limit,
             int64_t top_k,
             common::ObIArray<DocScore> &results);

  // ========== 并行任务管理 ==========
  /**
   * @brief 开始一个收集任务（在并行扫描开始时调用）
   */
  void begin_collect() { ATOMIC_AAF(&active_collectors_, 1); }
  
  /**
   * @brief 结束一个收集任务（在并行扫描结束时调用）
   * @return 是否所有任务都已结束（可以 finalize）
   */
  bool end_collect() { return ATOMIC_SAF(&active_collectors_, 1) == 0; }
  
  /**
   * @brief 检查是否可以 finalize（所有收集任务都已结束）
   */
  bool can_finalize() const { return ATOMIC_LOAD(&active_collectors_) == 0; }
  
  // ========== 状态查询 ==========
  bool is_inited() const { return is_inited_; }
  bool is_finalized() const { return is_finalized_; }
  bool is_ready() const { return is_inited_ && is_finalized_; }
  int64_t get_total_docs() const { return total_docs_; }
  int64_t get_vocab_size() const { return token_to_id_.size(); }
  int64_t get_total_entries() const { return scores_.count(); }

  void print_statistics() const;
  
  // ========== 调试接口 ==========
  /**
   * @brief 导出 CSC 索引到文件用于验证
   */
  void dump_csc_to_file(const char *filepath) const;
  
  /**
   * @brief 导出 finalize 耗时到文件
   * @param sort_file_size 排序时的文件大小（字节）
   * @param sort_mem_increase 排序导致的内存增长（KB）
   */
  void dump_finalize_time(const char *filepath, double elapsed_ms,
                          int64_t sort_file_size = 0, int64_t sort_mem_increase = 0) const;

private:
  ObFtsBM25SIndex();
  ~ObFtsBM25SIndex();

  int ensure_inited();
  double calc_bm25_score(int32_t tf, int32_t doc_len, int64_t df);

private:
  bool is_inited_;
  bool is_finalized_;
  volatile int64_t active_collectors_;  // 活跃的收集任务数（原子计数器）
  
  // ========== 收集阶段数据（分桶文件流式）==========
  FILE *bucket_files_[NUM_BUCKETS];       // 分桶临时文件
  char bucket_dir_path_[256];             // 桶文件目录路径
  volatile int64_t bucket_entry_counts_[NUM_BUCKETS]; // 每个桶的条目计数（原子）
  TokenIdMap collect_token_to_id_;        // 收集阶段的 token -> token_id
  DocIdSet unique_docs_;                  // 唯一文档 ID 集合
  volatile int64_t total_doc_len_;        // 总文档长度（原子）
  volatile int64_t raw_entry_count_;      // 总条目计数（原子）
  
  // ========== 细粒度锁 ==========
  mutable common::SpinRWLock token_map_lock_;    // HashMap 读写锁
  mutable common::SpinRWLock doc_set_lock_;      // DocSet 读写锁
  mutable common::ObSpinLock bucket_locks_[NUM_BUCKETS];  // 每个桶一把锁
  
  // ========== 最终 CSC 索引 ==========
  common::ObArray<float> scores_;        // 预计算得分
  common::ObArray<uint32_t> doc_ids_;    // doc_id（每个 token 内有序）
  common::ObArray<uint64_t> token_ptr_;  // 每个 token 的起止位置
  TokenIdMap token_to_id_;               // token -> token_id
  
  // ========== 统计信息 ==========
  int64_t total_docs_;            // 总文档数
  double avg_doc_len_;            // 平均文档长度
  
  // ========== 内存管理 ==========
  common::ObArenaAllocator allocator_;
  
  // ========== 并发控制 ==========
  mutable common::SpinRWLock lock_;

  DISALLOW_COPY_AND_ASSIGN(ObFtsBM25SIndex);
};

// ========== 模板实现（分桶文件流式）==========
template <typename FTWordMapType>
int ObFtsBM25SIndex::collect_document(int64_t doc_id, int32_t doc_len, const FTWordMapType &ft_word_map)
{
  int ret = OB_SUCCESS;
  
  if (ft_word_map.size() == 0) {
    // 空文档，跳过
  } else {
    // 遍历文档的所有 token，调用 collect_raw_data（内部有细粒度锁）
    for (typename FTWordMapType::const_iterator iter = ft_word_map.begin();
         OB_SUCC(ret) && iter != ft_word_map.end(); ++iter) {
      const common::ObString &token = iter->first.get_word().get_string();
      int32_t tf = static_cast<int32_t>(iter->second);
      
      if (!token.empty()) {
        ret = collect_raw_data(token, doc_id, tf, doc_len);
      }
    }
  }
  
  return ret;
}

// 全局单例宏
#define g_bm25s_index (::oceanbase::sql::ObFtsBM25SIndex::get_instance())

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_BM25S_INDEX_H_
