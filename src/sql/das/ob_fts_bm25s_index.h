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
  int32_t version;             // 版本号，V2 设计为 2
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
  int32_t bucket_num;          // 桶数量（V2 默认 1024）
  int32_t reserved3;
  int64_t reserved4;
  
  // 校验和 (16 bytes)
  uint64_t checksum;           // 文件校验和（可选）
  int64_t create_timestamp;    // 创建时间戳
  
  // 填充到 256 bytes
  char padding[256 - 144];
  
  // 常量
  static const uint64_t MAGIC = 0x424D32355349445A;  // "BM25SIDZ"
  static const int32_t CURRENT_VERSION = 2;  // V2 设计
  static const int64_t HEADER_SIZE = 256;
  static const int32_t DEFAULT_BUCKET_NUM = 1024;  // V2: 统一 1024 个桶
  
  void reset() { memset(this, 0, sizeof(*this)); }
  bool is_valid() const { return magic == MAGIC && version == CURRENT_VERSION; }  // 只支持 V2
  
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
 * V2 设计：移除了 token_id 字段
 * V3 设计：添加 min_doc_id 用于 token 前过滤
 * V4 设计：添加 max_doc_id 用于通用性（预留）
 */
struct VocabEntryMem {
  uint64_t token_hash;
  common::ObString token;
  int64_t posting_offset;
  int32_t posting_count;
  uint32_t min_doc_id;      // token 对应的最小文档 ID（用于前过滤）
  uint32_t max_doc_id;      // token 对应的最大文档 ID（预留）
  
  TO_STRING_KV(K(token_hash), K(token), K(posting_offset), K(posting_count), K(min_doc_id), K(max_doc_id));
};

/**
 * @brief BM25S 预计算索引 (V2 设计)
 * 
 * 三阶段构建：
 * 1. 收集阶段：DDL 时收集 (token, doc_id, tf, doc_len) 到 TmpFile 分桶
 * 2. Finalize 阶段：计算 BM25 得分，直接写入磁盘文件（不构建内存索引）
 * 3. 查询阶段：从磁盘文件按需加载，通过 KVCache 缓存热点数据
 * 
 * 磁盘文件格式：
 * - Header (256 bytes)
 * - Bucket Directory (16KB)
 * - Vocab Buckets (变长)
 * - Posting Data (doc_ids + scores)
 */
class ObFtsBM25SIndex
{
public:
  // ========== 查询结果 ==========
  struct DocScore
  {
    int64_t doc_id;
    float score;
    
    DocScore() : doc_id(0), score(0.0f) {}
    DocScore(int64_t d, float s) : doc_id(d), score(s) {}
    
    bool operator>(const DocScore &other) const { return score > other.score; }
    
    TO_STRING_KV(K(doc_id), K(score));
  };

  // ========== V2: 变长 TmpFile 记录 ==========
  // 格式: [token_len:2][token_str:N][doc_id:8][tf:4][doc_len:4]
  // 总大小: 18 + token_len 字节
  struct TmpRecord {
    common::ObString token;
    int64_t doc_id;
    int32_t tf;
    int32_t doc_len;
    
    TmpRecord() : doc_id(0), tf(0), doc_len(0) {}
    
    bool operator<(const TmpRecord &other) const {
      int cmp = token.compare(other.token);
      if (cmp != 0) return cmp < 0;
      return doc_id < other.doc_id;
    }
    
    TO_STRING_KV(K(token), K(doc_id), K(tf), K(doc_len));
  };
  
  typedef common::hash::ObHashSet<int64_t, common::hash::NoPthreadDefendMode> DocIdSet;

  static const int64_t DEFAULT_BUCKET_COUNT = 100000;
  static const int64_t NUM_BUCKETS = 1024;  // 分桶数量（使用 TmpFile），减少 WBP 内存压力
  static const int64_t DEFAULT_POSTING_LRU_CAPACITY = 20000;  // Posting LRU 默认容量
  
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

  // ========== 查询接口 ==========

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
  int64_t get_total_docs() const { return total_docs_; }
  int64_t get_vocab_size() const { return file_header_.vocab_size; }
  int64_t get_total_entries() const { return file_header_.total_postings; }

  void print_statistics() const;

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
  int get_posting_from_lru(const common::ObString &token,
                           const ObBM25SPostingValue *&posting);
  
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
  
  /**
   * @brief 确保索引就绪（从磁盘加载）
   * @return OB_SUCCESS 如果索引已就绪或成功加载
   */
  int ensure_ready();

  void set_use_posting_hashmap(bool v) { use_posting_hashmap_ = v; }
  bool is_use_posting_hashmap() const { return use_posting_hashmap_; }
  void set_posting_lru_capacity(int64_t capacity) { posting_lru_capacity_ = capacity; }
  int64_t get_posting_lru_capacity() const { return posting_lru_capacity_; }
  int64_t get_posting_lru_size() const { return posting_lru_size_; }

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
   * @brief V2: 写入变长记录到指定桶
   * @param bucket_id 桶 ID [0, NUM_BUCKETS)
   * @param token token 字符串
   * @param doc_id 文档 ID
   * @param tf 词频
   * @param doc_len 文档长度
   */
  int write_variable_record(int64_t bucket_id, const common::ObString &token,
                            int64_t doc_id, int32_t tf, int32_t doc_len);
  
  /**
   * @brief V2: 读取整个桶的所有变长记录
   * @param bucket_id 桶 ID [0, NUM_BUCKETS)
   * @param records 输出：读取的记录数组
   * @param out_buf 输出：数据缓冲区指针（调用方负责释放）
   */
  int read_variable_bucket(int64_t bucket_id, common::ObIArray<TmpRecord> &records, char *&out_buf);
  
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
  int load_posting_to_lru(const common::ObString &token,
                          const ObBM25SPostingValue *&posting,
                          uint64_t token_hash = 0);
  int init_posting_lru();
  
  /**
   * @brief 从文件查找 Token 的 posting 信息
   * @param token 查询词
   * @param posting_offset 输出：posting 在文件中的偏移
   * @param posting_count 输出：posting 数量
   * @param min_doc_id 输出：token 的最小文档 ID（用于前过滤）
   * @param max_doc_id 输出：token 的最大文档 ID（预留）
   */
  int find_token_in_file(const common::ObString &token,
                         int64_t &posting_offset,
                         int64_t &posting_count,
                         uint32_t &min_doc_id,
                         uint32_t &max_doc_id);

  // ========== V3: LRU ObHashMap 缓存结构 ==========
  struct PostingHashKey
  {
    PostingHashKey() : hash_(0), len_(0), ptr_(nullptr) {}
    PostingHashKey(uint64_t hash, int32_t len, const char *ptr)
        : hash_(hash), len_(len), ptr_(ptr) {}
    uint64_t hash() const { return hash_; }
    int hash(uint64_t &hash_val) const { hash_val = hash_; return common::OB_SUCCESS; }
    bool operator==(const PostingHashKey &other) const
    {
      return hash_ == other.hash_
             && len_ == other.len_
             && (len_ == 0 || (nullptr != ptr_ && nullptr != other.ptr_ && 0 == MEMCMP(ptr_, other.ptr_, len_)));
    }
    uint64_t hash_;
    int32_t len_;
    const char *ptr_;
  };

  struct PostingLRUNode {
    PostingHashKey key;
    common::ObString token;
    const ObBM25SPostingValue *posting;
    PostingLRUNode *prev;
    PostingLRUNode *next;

    PostingLRUNode() : key(), token(), posting(nullptr), prev(nullptr), next(nullptr) {}
  };
  typedef common::hash::ObHashMap<PostingHashKey, PostingLRUNode*, common::hash::NoPthreadDefendMode> PostingLRUMap;

  void posting_lru_add_to_head(PostingLRUNode *node);
  void posting_lru_remove_node(PostingLRUNode *node);
  void posting_lru_evict_oldest();

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
  char index_file_path_[512];                       // 索引文件路径
  common::ObIOFd persist_fd_;                       // 持久化文件描述符（写入/查询共用）
  bool persist_file_opened_;                        // 持久化文件是否已打开
  BM25SIndexFileHeader file_header_;                // 文件头
  BucketDirectoryEntry *bucket_directory_;          // 桶目录
  
  // ========== Phase 3: KVCache / LRU 查询相关 ==========
  common::ObArenaAllocator cache_allocator_;        // KVCache 查询时的临时内存分配器
  common::ObArenaAllocator posting_lru_allocator_;  // Posting LRU 使用的持久内存
  
  // ========== 收集阶段数据（分桶文件流式）==========
  volatile int64_t bucket_entry_counts_[NUM_BUCKETS]; // 每个桶的条目计数（原子）
  DocIdSet unique_docs_;                  // 唯一文档 ID 集合
  volatile int64_t total_doc_len_;        // 总文档长度（原子）
  volatile int64_t raw_entry_count_;      // 总条目计数（原子）
  
  // ========== 细粒度锁 ==========
  mutable common::SpinRWLock doc_set_lock_;      // DocSet 读写锁
  mutable common::ObSpinLock bucket_locks_[NUM_BUCKETS];  // 每个桶一把锁（TmpFile 写入保护）
  
  // ========== 统计信息 ==========
  int64_t total_docs_;            // 总文档数
  double avg_doc_len_;            // 平均文档长度
  
  // ========== 内存管理 ==========
  common::ObArenaAllocator allocator_;
  
  // ========== 并发控制 ==========
  mutable common::SpinRWLock lock_;
  PostingLRUMap posting_lru_map_;
  PostingLRUNode posting_lru_head_;
  PostingLRUNode posting_lru_tail_;
  int64_t posting_lru_size_;
  int64_t posting_lru_capacity_;
  bool posting_lru_inited_;
  bool use_posting_hashmap_;
  mutable common::SpinRWLock posting_lru_lock_;

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
