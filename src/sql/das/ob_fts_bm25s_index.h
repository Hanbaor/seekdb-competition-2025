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
#include "storage/tmp_file/ob_tmp_file_manager.h"
#include "storage/tmp_file/ob_tmp_file_io_info.h"
#include "storage/tmp_file/ob_tmp_file_io_handle.h"
#include "share/ob_io_device_helper.h"
#include "common/storage/ob_io_device.h"
#include "share/cache/ob_kv_storecache.h"
#include <fstream>
#include <cstdio>
#include <fcntl.h>

// 前向声明
namespace oceanbase {
namespace sql {
class ObBM25SPostingValue;
class ObBM25SPostingKey;
} // namespace sql
} // namespace oceanbase

namespace oceanbase
{
namespace sql
{

// 持久化日志函数（定义在 ob_fts_bm25s_index.cpp）
void bm25s_log(const char* fmt, ...);

// ========== Phase 2: 持久化文件格式结构体 ==========

/**
 * @brief BM25S 索引文件头 (256 bytes)
 */
struct BM25SIndexFileHeader {
  // Magic & Version (16 bytes)
  uint64_t magic;              // 0x424D32355349445A ("BM25SIDZ")
  int32_t version;             // 版本号，当前为 1
  int32_t flags;               // 预留标志位
  
  // 统计信息 (32 bytes)
  int64_t total_docs;          // 总文档数
  double avg_doc_len;          // 平均文档长度
  int64_t vocab_size;          // 词汇表大小（token 数量）
  int64_t total_postings;      // posting 总条目数
  
  // Section 偏移和大小 (64 bytes)
  int64_t bucket_dir_offset;   // Bucket Directory 起始偏移
  int64_t bucket_dir_size;     // Bucket Directory 大小
  int64_t vocab_buckets_offset;// Vocab Buckets 起始偏移
  int64_t vocab_buckets_size;  // Vocab Buckets 总大小
  int64_t posting_data_offset; // Posting Data 起始偏移
  int64_t posting_data_size;   // Posting Data 总大小
  int64_t reserved1;
  int64_t reserved2;
  
  // 配置参数 (16 bytes)
  int32_t bucket_num;          // 桶数量（默认 4096）
  int32_t reserved3;
  int64_t reserved4;
  
  // 校验和 (16 bytes)
  uint64_t checksum;           // 文件校验和（可选）
  int64_t create_timestamp;    // 创建时间戳
  
  // 填充到 256 bytes
  char padding[256 - 144];
  
  // 常量
  static const uint64_t MAGIC = 0x424D32355349445A;  // "BM25SIDZ"
  static const int32_t CURRENT_VERSION = 1;
  static const int64_t HEADER_SIZE = 256;
  static const int32_t DEFAULT_BUCKET_NUM = 4096;
  
  void reset() { memset(this, 0, sizeof(*this)); }
  bool is_valid() const { return magic == MAGIC && version == CURRENT_VERSION; }
  
  TO_STRING_KV(K(magic), K(version), K(total_docs), K(avg_doc_len), K(vocab_size), 
               K(total_postings), K(bucket_dir_offset), K(vocab_buckets_offset), 
               K(posting_data_offset), K(posting_data_size));
};

/**
 * @brief 桶目录条目 (16 bytes)
 */
struct BucketDirectoryEntry {
  int64_t offset;      // 该桶在文件中的偏移（相对于 vocab_buckets_offset）
  int32_t entry_count; // 该桶包含的 token 数量
  int32_t size;        // 该桶的数据大小（字节）
  
  void reset() { offset = 0; entry_count = 0; size = 0; }
  
  TO_STRING_KV(K(offset), K(entry_count), K(size));
};

/**
 * @brief Vocab Entry 内存结构（用于写入）
 */
struct VocabEntryMem {
  uint64_t token_hash;
  common::ObString token;
  uint32_t token_id;
  int64_t posting_offset;
  int32_t posting_count;
  
  TO_STRING_KV(K(token_hash), K(token), K(token_id), K(posting_offset), K(posting_count));
};

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
    
    TO_STRING_KV(K(token_id), K(doc_id), K(tf), K(doc_len));
  } __attribute__((packed));  // 20 字节
  typedef common::hash::ObHashMap<common::ObString, uint32_t,
                                   common::hash::NoPthreadDefendMode> TokenIdMap;
  typedef common::hash::ObHashSet<int64_t, common::hash::NoPthreadDefendMode> DocIdSet;

  static const int64_t DEFAULT_BUCKET_COUNT = 100000;
  static const int64_t NUM_BUCKETS = 1024;  // 分桶数量（使用 TmpFile），用于减少排序内存峰值
  
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

  /**
   * @brief 搜索查询（全局数组优化版本，使用锁）
   */
  int search_with_array(const common::ObIArray<common::ObString> &tokens,
                        int64_t id_limit,
                        int64_t top_k,
                        common::ObIArray<DocScore> &results);

  /**
   * @brief 获取某个 token 的倒排链（支持 id_limit 过滤）
   * @param token 查询词
   * @param id_limit 只返回 doc_id < id_limit 的部分（0 表示不过滤）
   * @param start 输出：倒排链起始索引
   * @param end 输出：倒排链结束索引（不包含）
   * @return true 如果 token 存在
   */
  bool get_posting_list(const common::ObString &token, int64_t id_limit, uint64_t &start, uint64_t &end) const;
  
  // 直接访问倒排链数据
  uint32_t get_doc_id(uint64_t idx) const { return doc_ids_.at(idx); }
  float get_score(uint64_t idx) const { return scores_.at(idx); }

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
  bool is_ready() const { return is_finalized_; }  // 只需 finalized，不需要 inited
  
  /**
   * @brief 确保索引就绪（如果未就绪，尝试从 TmpFile 加载）
   * 在搜索时调用
   */
  int ensure_ready();
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

  // ========== Phase 2: 持久化接口 ==========
  /**
   * @brief 获取索引文件路径
   * @param tenant_id 租户 ID
   * @param table_id 表 ID
   * @param path_buf 输出路径缓冲区
   * @param buf_len 缓冲区长度
   */
  static int get_index_path(uint64_t tenant_id, uint64_t table_id,
                            char *path_buf, int64_t buf_len);
  
  /**
   * @brief 将索引持久化到磁盘
   * @param index_dir 索引目录路径
   */
  int persist_to_disk(const char *index_dir);
  
  /**
   * @brief 流式 Finalize + 持久化（不构建内存 CSC，直接写文件）
   * @param index_path 索引文件完整路径
   */
  int finalize_and_persist(const char *index_path);
  
  /**
   * @brief 从磁盘加载索引
   * @param index_dir 索引目录路径
   */
  int load_from_disk(const char *index_dir);
  
  /**
   * @brief 关闭索引文件
   */
  void close_index_file();

  // ========== Phase 3: KVCache 查询接口 ==========
  /**
   * @brief 使用 KVCache 进行搜索（支持动态加载）
   * @param tokens 查询词列表
   * @param id_limit id < id_limit 过滤条件（0 表示不过滤）
   * @param top_k 返回 top_k 个结果
   * @param results 输出结果（doc_id, score）
   */
  int search_with_cache(const common::ObIArray<common::ObString> &tokens,
                        int64_t id_limit,
                        int64_t top_k,
                        common::ObIArray<DocScore> &results);
  
  /**
   * @brief 从 PostingCache 获取倒排链
   * @param token 查询词
   * @param posting 输出：posting 数据指针
   * @param handle 缓存句柄（管理生命周期）
   */
  int get_posting_from_cache(const common::ObString &token,
                             const ObBM25SPostingValue *&posting,
                             common::ObKVCacheHandle &handle);
  
  /**
   * @brief 打印缓存统计信息
   */
  void print_cache_stats();
  
  /**
   * @brief 检查索引文件是否已打开
   */
  bool is_index_file_opened() const { return persist_file_opened_; }
  
  /**
   * @brief 获取文件头信息（用于查询时访问元数据）
   */
  const BM25SIndexFileHeader& get_file_header() const { return file_header_; }

private:
  ObFtsBM25SIndex();
  ~ObFtsBM25SIndex();

  int ensure_inited();
  double calc_bm25_score(int32_t tf, int32_t doc_len, int64_t df);
  
  // ========== TmpFile 接口（Phase 1）==========
  /**
   * @brief 初始化 TmpFile 目录和文件
   */
  int init_tmp_files();
  
  /**
   * @brief 写入一条记录到指定桶
   * @param bucket_id 桶 ID [0, NUM_BUCKETS)
   * @param record 要写入的记录
   */
  int write_to_bucket(int64_t bucket_id, const FileRecord &record);
  
  /**
   * @brief 批量写入多条记录到指定桶（减少系统调用）
   * @param bucket_id 桶 ID [0, NUM_BUCKETS)
   * @param records 记录数组
   * @param count 记录数量
   */
  int write_records_to_bucket(int64_t bucket_id, const FileRecord *records, int64_t count);
  
  /**
   * @brief 读取整个桶的所有记录
   * @param bucket_id 桶 ID [0, NUM_BUCKETS)
   * @param records 输出：读取的记录数组
   */
  int read_bucket(int64_t bucket_id, common::ObIArray<FileRecord> &records);
  
  /**
   * @brief 清理所有 TmpFile
   */
  void cleanup_tmp_files();
  

  
  // ========== Phase 2: 持久化私有函数 ==========
  /**
   * @brief 写入 Header 和 Bucket Directory（占位）
   */
  int write_header_and_directory(const common::ObIOFd &fd);
  
  /**
   * @brief 预计算 Posting Offsets
   */
  int calc_posting_offsets(common::ObIArray<int64_t> &posting_offsets);
  
  /**
   * @brief 写入 Vocab Buckets
   */
  int write_vocab_buckets(const common::ObIOFd &fd,
                          const common::ObIArray<int64_t> &posting_offsets);
  
  /**
   * @brief 写入 Vocab Buckets（流式版本，用于 finalize_and_persist）
   */
  int write_vocab_buckets_streaming(const common::ObIOFd &fd,
                                     const common::ObIArray<int64_t> &posting_offsets,
                                     const common::ObIArray<int64_t> &token_df);
  
  /**
   * @brief 写入 Posting Data
   */
  int write_posting_data(const common::ObIOFd &fd);
  
  /**
   * @brief 更新 Header 和 Bucket Directory
   */
  int update_header(const common::ObIOFd &fd);
  
  /**
   * @brief 计算 BM25 分数（float 版本，用于持久化）
   */
  float calc_bm25_score_f(int32_t tf, int32_t doc_len, int64_t df);
  
  // ========== Phase 3: KVCache 私有函数 ==========
  /**
   * @brief 从文件加载 Posting 并放入缓存
   * @param token 查询词
   * @param posting 输出：posting 数据指针
   * @param handle 缓存句柄
   */
  int load_posting_to_cache(const common::ObString &token,
                            const ObBM25SPostingValue *&posting,
                            common::ObKVCacheHandle &handle);
  
  /**
   * @brief 从文件查找 Token 的 posting 信息
   * @param token 查询词
   * @param posting_offset 输出：posting 在文件中的偏移
   * @param posting_count 输出：posting 数量
   */
  int find_token_in_file(const common::ObString &token,
                         int64_t &posting_offset,
                         int64_t &posting_count);

private:
  bool is_inited_;
  bool is_finalized_;
  volatile int64_t active_collectors_;  // 活跃的收集任务数（原子计数器）
  
  // ========== TmpFile 分桶存储（Phase 1）==========
  int64_t tmp_dir_id_;                            // TmpFile 目录 ID
  int64_t bucket_fds_[NUM_BUCKETS];               // TmpFile 文件描述符数组
  volatile int64_t bucket_write_offsets_[NUM_BUCKETS]; // 每个桶的当前写入偏移（用于统计）
  bool tmp_files_inited_;                         // TmpFile 是否已初始化
  
  // ========== Phase 2: 持久化相关 ==========
  static const int32_t NUM_PERSIST_BUCKETS = 4096;  // 持久化文件的桶数量
  char index_file_path_[512];                       // 索引文件路径
  common::ObIOFd persist_fd_;                       // 持久化文件描述符（写入/查询共用）
  bool persist_file_opened_;                        // 持久化文件是否已打开
  BM25SIndexFileHeader file_header_;                // 文件头
  BucketDirectoryEntry *bucket_directory_;          // 桶目录
  
  // ========== Phase 3: KVCache 查询相关 ==========
  common::ObArenaAllocator cache_allocator_;        // KVCache 查询时的临时内存分配器
  
  // ========== 收集阶段数据（分桶文件流式）==========
  volatile int64_t bucket_entry_counts_[NUM_BUCKETS]; // 每个桶的条目计数（原子）
  TokenIdMap collect_token_to_id_;        // 收集阶段的 token -> token_id
  DocIdSet unique_docs_;                  // 唯一文档 ID 集合
  volatile int64_t total_doc_len_;        // 总文档长度（原子）
  volatile int64_t raw_entry_count_;      // 总条目计数（原子）
  
  // ========== 细粒度锁 ==========
  mutable common::SpinRWLock token_map_lock_;    // HashMap 读写锁
  mutable common::SpinRWLock doc_set_lock_;      // DocSet 读写锁
  mutable common::ObSpinLock bucket_locks_[NUM_BUCKETS];  // 每个桶一把锁（TmpFile 写入保护）
  
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
