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

#ifndef OCEANBASE_SQL_DAS_OB_FTS_TOKEN_DOC_CACHE_H_
#define OCEANBASE_SQL_DAS_OB_FTS_TOKEN_DOC_CACHE_H_

#include "lib/hash/ob_hashmap.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/lock/ob_spin_rwlock.h"
#include "share/datum/ob_datum.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Token到文档ID范围的映射信息
 */
struct ObTokenDocRange
{
public:
  ObTokenDocRange()
    : min_doc_id_(INT64_MAX),
      max_doc_id_(INT64_MIN),
      doc_count_(0)
  {}

  void reset()
  {
    min_doc_id_ = INT64_MAX;
    max_doc_id_ = INT64_MIN;
    doc_count_ = 0;
  }

  void update(int64_t doc_id)
  {
    if (doc_id < min_doc_id_) {
      min_doc_id_ = doc_id;
    }
    if (doc_id > max_doc_id_) {
      max_doc_id_ = doc_id;
    }
    doc_count_++;
  }

  void merge(const ObTokenDocRange &other)
  {
    if (other.min_doc_id_ < min_doc_id_) {
      min_doc_id_ = other.min_doc_id_;
    }
    if (other.max_doc_id_ > max_doc_id_) {
      max_doc_id_ = other.max_doc_id_;
    }
    doc_count_ += other.doc_count_;
  }

  TO_STRING_KV(K_(min_doc_id), K_(max_doc_id), K_(doc_count));

public:
  int64_t min_doc_id_;
  int64_t max_doc_id_;
  int64_t doc_count_;
};

// 前向声明
class ObFTSTokenDocCache;

/**
 * @brief 线程局部缓存
 */
class ObFTSThreadLocalCache
{
public:
  typedef common::hash::ObHashMap<common::ObString, ObTokenDocRange> TokenDocMap;
  static const int64_t DEFAULT_BUCKET_COUNT = 10000;

  ObFTSThreadLocalCache();
  ~ObFTSThreadLocalCache();

  int init();
  void reset();
  void destroy();
  bool is_inited() const { return is_inited_; }

  int add_token_doc_id(const common::ObString &token, int64_t doc_id);
  int get_token_doc_range(const common::ObString &token, ObTokenDocRange &doc_range) const;

  TokenDocMap &get_map() { return token_doc_map_; }
  const TokenDocMap &get_map() const { return token_doc_map_; }

private:
  bool is_inited_;
  TokenDocMap token_doc_map_;
  common::ObArenaAllocator allocator_;
};

/**
 * @brief 全局缓存管理器
 * - 写入时：每个线程写自己的局部缓存（无锁）
 * - 查询时：遍历所有线程的局部缓存（有锁，但只在查询时）
 */
class ObFTSTokenDocCache
{
public:
  typedef common::hash::ObHashMap<common::ObString, ObTokenDocRange> TokenDocMap;
  static const int64_t MAX_THREAD_CACHES = 256;

  static ObFTSTokenDocCache &get_instance();

  int init();
  void reset();
  void destroy();

  /**
   * @brief 添加 token（写入线程局部缓存，无锁）
   */
  int add_token_doc_id(const common::ObString &token, int64_t doc_id);
  int add_token_doc_id(const common::ObString &token, const common::ObDatum &doc_id_datum);

  /**
   * @brief 查询 token（遍历所有线程局部缓存）
   */
  int get_token_doc_range(const common::ObString &token, ObTokenDocRange &doc_range) const;
  bool contains_token(const common::ObString &token) const;
  int64_t get_token_count() const;
  bool is_inited() const { return is_inited_; }

  void print_statistics() const;
  int get_all_tokens(common::ObIArray<common::ObString> &tokens) const;

  /**
   * @brief 注册线程局部缓存（首次写入时自动调用）
   */
  int register_thread_local_cache(ObFTSThreadLocalCache *cache);

private:
  ObFTSTokenDocCache();
  ~ObFTSTokenDocCache();

  int ensure_inited();
  ObFTSThreadLocalCache *get_or_create_thread_local_cache();

private:
  bool is_inited_;
  
  // 所有线程局部缓存的指针数组
  ObFTSThreadLocalCache *thread_caches_[MAX_THREAD_CACHES];
  int64_t thread_cache_count_;
  mutable common::SpinRWLock registry_lock_;  // 只在注册/查询时用

  DISALLOW_COPY_AND_ASSIGN(ObFTSTokenDocCache);
};

#define FTS_TOKEN_DOC_CACHE (::oceanbase::sql::ObFTSTokenDocCache::get_instance())

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_TOKEN_DOC_CACHE_H_
