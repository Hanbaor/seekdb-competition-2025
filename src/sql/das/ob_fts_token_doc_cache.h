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
 * @brief Token到文档ID范围的映射信息（包含完整倒排索引）
 */
struct ObTokenDocRange
{
public:
  ObTokenDocRange()
    : min_doc_id_(INT64_MAX),
      max_doc_id_(INT64_MIN),
      doc_count_(0),
      doc_ids_(nullptr),
      allocator_(nullptr)
  {}

  ~ObTokenDocRange()
  {
    // ObArenaAllocator不支持单独释放，所以这里不需要手动释放
  }

  void reset()
  {
    min_doc_id_ = INT64_MAX;
    max_doc_id_ = INT64_MIN;
    doc_count_ = 0;
    if (doc_ids_ != nullptr) {
      doc_ids_->reset();
    }
  }

  int update(int64_t doc_id, common::ObIAllocator *alloc)
  {
    int ret = common::OB_SUCCESS;
    if (doc_id < min_doc_id_) {
      min_doc_id_ = doc_id;
    }
    if (doc_id > max_doc_id_) {
      max_doc_id_ = doc_id;
    }
    doc_count_++;
    
    // 添加到doc_ids_列表中
    if (nullptr == doc_ids_) {
      if (OB_ISNULL(alloc)) {
        ret = common::OB_INVALID_ARGUMENT;
      } else {
        allocator_ = alloc;
        void *buf = allocator_->alloc(sizeof(common::ObSEArray<int64_t, 16>));
        if (OB_ISNULL(buf)) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else {
          doc_ids_ = new (buf) common::ObSEArray<int64_t, 16>();
        }
      }
    }
    
    if (common::OB_SUCCESS == ret && doc_ids_ != nullptr) {
      if (common::OB_SUCCESS != doc_ids_->push_back(doc_id)) {
        ret = common::OB_ERR_UNEXPECTED;
      }
    }
    
    return ret;
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
    
    // 合并doc_ids_列表
    if (other.doc_ids_ != nullptr && doc_ids_ != nullptr) {
      for (int64_t i = 0; i < other.doc_ids_->count(); ++i) {
        doc_ids_->push_back(other.doc_ids_->at(i));
      }
    }
  }

  // 获取doc_ids数组（只读）
  const common::ObSEArray<int64_t, 16> *get_doc_ids() const { return doc_ids_; }
  bool has_full_inverted_index() const { return doc_ids_ != nullptr && doc_ids_->count() > 0; }

  TO_STRING_KV(K_(min_doc_id), K_(max_doc_id), K_(doc_count));

public:
  int64_t min_doc_id_;
  int64_t max_doc_id_;
  int64_t doc_count_;
  common::ObSEArray<int64_t, 16> *doc_ids_;  // 完整的倒排索引：doc_id列表（有序）
  common::ObIAllocator *allocator_;  // 用于分配doc_ids_
};

/**
 * @brief 全局 FTS Token-Doc 缓存（单锁版本）
 * - 单一全局 HashMap，禁用内部锁
 * - 使用单一全局锁保护所有操作
 */
class ObFTSTokenDocCache
{
public:
  // 使用 NoPthreadDefendMode 禁用 HashMap 内部锁，由我们自己管理锁
  typedef common::hash::ObHashMap<common::ObString, ObTokenDocRange,
                                   common::hash::NoPthreadDefendMode> TokenDocMap;
  static const int64_t DEFAULT_BUCKET_COUNT = 100000;

  static ObFTSTokenDocCache &get_instance();

  int init();
  void reset();
  void destroy();

  /**
   * @brief 添加 token 和 doc_id 到缓存（全局锁）
   */
  int add_token_doc_id(const common::ObString &token, int64_t doc_id);
  int add_token_doc_id(const common::ObString &token, const common::ObDatum &doc_id_datum);

  /**
   * @brief 查询 token 的 doc_id 范围（全局锁）
   */
  int get_token_doc_range(const common::ObString &token, ObTokenDocRange &doc_range) const;
  bool contains_token(const common::ObString &token) const;
  int64_t get_token_count() const;
  bool is_inited() const { return is_inited_; }

  void print_statistics() const;
  int get_all_tokens(common::ObIArray<common::ObString> &tokens) const;

private:
  ObFTSTokenDocCache();
  ~ObFTSTokenDocCache();

  int ensure_inited();

private:
  bool is_inited_;
  TokenDocMap token_doc_map_;
  common::ObArenaAllocator allocator_;
  
  // 单一全局锁保护所有操作
  mutable common::SpinRWLock lock_;

  DISALLOW_COPY_AND_ASSIGN(ObFTSTokenDocCache);
};

#define FTS_TOKEN_DOC_CACHE (::oceanbase::sql::ObFTSTokenDocCache::get_instance())

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_TOKEN_DOC_CACHE_H_
