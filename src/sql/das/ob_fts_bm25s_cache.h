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

#ifndef OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_
#define OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_

#include "share/cache/ob_kvcache_struct.h"
#include "share/cache/ob_kv_storecache.h"
#include "lib/string/ob_string.h"
#include "lib/hash/ob_hashutils.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

// ========================================================================
// Phase 3: BM25S KVCache 定义
// 一级缓存：PostingCache - 直接 token → posting
// 注意：当前比赛场景为全局单例，无需 tenant_id/table_id
// ========================================================================

/**
 * @brief PostingCache Key - 使用 token 字符串作为 Key
 * 完整比较 token 字符串，避免 hash 碰撞
 */
class ObBM25SPostingKey : public common::ObIKVCacheKey
{
public:
  ObBM25SPostingKey()
      : token_len_(0), token_data_(nullptr) {}
  
  ObBM25SPostingKey(const common::ObString &token)
      : token_len_(token.length()), token_data_(const_cast<char*>(token.ptr())) {}
  
  virtual ~ObBM25SPostingKey() {}
  
  // 比较操作符（完整比较 token 字符串，避免 hash 碰撞）
  virtual bool operator==(const ObIKVCacheKey &other) const override
  {
    const ObBM25SPostingKey &o = static_cast<const ObBM25SPostingKey&>(other);
    if (token_len_ != o.token_len_) {
      return false;
    }
    if (token_len_ == 0) {
      return true;
    }
    return 0 == MEMCMP(token_data_, o.token_data_, token_len_);
  }
  
  // Hash 函数
  virtual uint64_t hash() const override
  {
    if (token_len_ > 0 && OB_NOT_NULL(token_data_)) {
      return common::murmurhash(token_data_, token_len_, 0);
    }
    return 0;
  }
  
  // 返回系统租户（全局单例模式）
  virtual uint64_t get_tenant_id() const override { return OB_SYS_TENANT_ID; }
  
  virtual int64_t size() const override 
  { 
    return sizeof(*this) + token_len_; 
  }
  
  // 深拷贝（包括 token 字符串）
  virtual int deep_copy(char *buf, const int64_t buf_len,
                        ObIKVCacheKey *&key) const override
  {
    int ret = OB_SUCCESS;
    int64_t need_size = size();
    
    if (OB_ISNULL(buf) || buf_len < need_size) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObBM25SPostingKey *new_key = new (buf) ObBM25SPostingKey();
      new_key->token_len_ = token_len_;
      
      // token 数据紧随对象之后
      char *token_buf = buf + sizeof(ObBM25SPostingKey);
      if (token_len_ > 0 && OB_NOT_NULL(token_data_)) {
        MEMCPY(token_buf, token_data_, token_len_);
      }
      new_key->token_data_ = token_buf;
      
      key = new_key;
    }
    return ret;
  }
  
  common::ObString get_token() const 
  { 
    return common::ObString(token_len_, token_data_); 
  }
  
  TO_STRING_KV(K_(token_len));
  
public:
  int32_t token_len_;
  char *token_data_;
};

/**
 * @brief PostingCache Value - 存储 posting list (doc_ids + scores)
 */
class ObBM25SPostingValue : public common::ObIKVCacheValue
{
public:
  ObBM25SPostingValue()
      : count_(0), doc_ids_(nullptr), scores_(nullptr) {}
  
  virtual ~ObBM25SPostingValue() {}
  
  // 设置数据
  void set(int64_t count, const uint32_t *doc_ids, const float *scores)
  {
    count_ = count;
    doc_ids_ = const_cast<uint32_t*>(doc_ids);
    scores_ = const_cast<float*>(scores);
  }
  
  // 获取数据大小
  virtual int64_t size() const override
  {
    return sizeof(*this) + count_ * (sizeof(uint32_t) + sizeof(float));
  }
  
  // 深拷贝
  virtual int deep_copy(char *buf, const int64_t buf_len,
                        ObIKVCacheValue *&value) const override
  {
    int ret = OB_SUCCESS;
    int64_t need_size = size();
    
    if (OB_ISNULL(buf) || buf_len < need_size) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObBM25SPostingValue *new_val = new (buf) ObBM25SPostingValue();
      new_val->count_ = count_;
      
      char *data_buf = buf + sizeof(ObBM25SPostingValue);
      
      // 复制 doc_ids
      new_val->doc_ids_ = reinterpret_cast<uint32_t*>(data_buf);
      if (count_ > 0 && OB_NOT_NULL(doc_ids_)) {
        MEMCPY(new_val->doc_ids_, doc_ids_, count_ * sizeof(uint32_t));
      }
      data_buf += count_ * sizeof(uint32_t);
      
      // 复制 scores
      new_val->scores_ = reinterpret_cast<float*>(data_buf);
      if (count_ > 0 && OB_NOT_NULL(scores_)) {
        MEMCPY(new_val->scores_, scores_, count_ * sizeof(float));
      }
      
      value = new_val;
    }
    return ret;
  }
  
  // ========== 查询辅助函数 ==========
  
  // 获取 posting 数量
  int64_t count() const { return count_; }
  
  // 获取指定位置的 doc_id
  uint32_t get_doc_id(int64_t idx) const 
  { 
    return (idx >= 0 && idx < count_) ? doc_ids_[idx] : 0; 
  }
  
  // 获取指定位置的 score
  float get_score(int64_t idx) const 
  { 
    return (idx >= 0 && idx < count_) ? scores_[idx] : 0.0f; 
  }
  
  // 获取 doc_ids 数组指针
  const uint32_t* get_doc_ids() const { return doc_ids_; }
  
  // 获取 scores 数组指针
  const float* get_scores() const { return scores_; }
  
  // ========== 热数据二分查找支持 ==========
  // doc_ids 在构建时已按升序排列，支持 O(log n) 二分查找
  
  /**
   * @brief 二分查找：找到第一个 doc_id >= limit 的位置
   * 用于 id_limit 过滤优化
   */
  int64_t find_first_ge(int64_t limit) const
  {
    if (count_ == 0 || limit <= 0) {
      return 0;
    }
    
    // 快速判断
    if (doc_ids_[0] >= static_cast<uint32_t>(limit)) {
      return 0;  // 第一个就 >= limit
    }
    if (doc_ids_[count_ - 1] < static_cast<uint32_t>(limit)) {
      return count_;  // 所有都 < limit
    }
    
    // 二分查找
    int64_t lo = 0, hi = count_;
    while (lo < hi) {
      int64_t mid = lo + (hi - lo) / 2;
      if (doc_ids_[mid] < static_cast<uint32_t>(limit)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }
  
  /**
   * @brief 获取 doc_id < id_limit 的有效数量（二分查找，O(log n)）
   */
  int64_t get_valid_count(int64_t id_limit) const
  {
    if (id_limit <= 0 || count_ == 0) {
      return count_;  // 无过滤，返回全部
    }
    return find_first_ge(id_limit);
  }
  
  TO_STRING_KV(K_(count));
  
public:
  int64_t count_;
  uint32_t *doc_ids_;
  float *scores_;
};

// ========================================================================
// KVCache 类型定义（一级缓存）
// ========================================================================

typedef common::ObKVCache<ObBM25SPostingKey, ObBM25SPostingValue> ObBM25SPostingCache;

// ========================================================================
// 全局缓存管理函数声明
// ========================================================================

/**
 * @brief 初始化 BM25S Posting 缓存
 */
int init_bm25s_cache();

/**
 * @brief 销毁 BM25S Posting 缓存
 */
void destroy_bm25s_cache();

/**
 * @brief 检查缓存是否已初始化
 */
bool is_bm25s_cache_inited();

/**
 * @brief 获取全局 PostingCache 实例
 */
ObBM25SPostingCache* get_bm25s_posting_cache();

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_

