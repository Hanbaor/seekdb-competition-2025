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

#include "sql/das/ob_fts_token_doc_cache.h"
#include "lib/ob_define.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

// 静态常量定义
const int64_t ObFTSTokenDocCache::DEFAULT_BUCKET_COUNT;

// 用于原子更新的回调函数
struct UpdateTokenDocRangeCallback
{
  int64_t doc_id_;
  ObTokenDocRange *result_range_;  // 用于返回更新后的范围
  
  UpdateTokenDocRangeCallback(int64_t doc_id, ObTokenDocRange *result = nullptr)
    : doc_id_(doc_id), result_range_(result) {}
  
  void operator()(hash::HashMapPair<ObString, ObTokenDocRange> &pair)
  {
    pair.second.update(doc_id_);
    if (result_range_ != nullptr) {
      *result_range_ = pair.second;
    }
  }
};

// ==================== ObFTSTokenDocCache ====================

ObFTSTokenDocCache::ObFTSTokenDocCache()
  : is_inited_(false),
    token_doc_map_(),
    allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "FTSCache")),
    lock_()
{
}

ObFTSTokenDocCache::~ObFTSTokenDocCache()
{
  destroy();
}

ObFTSTokenDocCache &ObFTSTokenDocCache::get_instance()
{
  static ObFTSTokenDocCache instance;
  return instance;
}

int ObFTSTokenDocCache::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    // 已初始化
  } else {
    SpinWLockGuard guard(lock_);
    if (!is_inited_) {
      if (OB_FAIL(token_doc_map_.create(DEFAULT_BUCKET_COUNT,
                                         ObMemAttr(OB_SERVER_TENANT_ID, "FTSCacheMap")))) {
        LOG_WARN("fail to create token doc map", K(ret));
      } else {
        is_inited_ = true;
        LOG_INFO("ObFTSTokenDocCache initialized (single-lock mode)");
      }
    }
  }
  return ret;
}

int ObFTSTokenDocCache::ensure_inited()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = init();
  }
  return ret;
}

void ObFTSTokenDocCache::reset()
{
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    token_doc_map_.reuse();
    allocator_.reset();
    LOG_INFO("ObFTSTokenDocCache reset");
  }
}

void ObFTSTokenDocCache::destroy()
{
  SpinWLockGuard guard(lock_);
  
  if (is_inited_) {
    token_doc_map_.destroy();
    allocator_.reset();
    is_inited_ = false;
    LOG_INFO("ObFTSTokenDocCache destroyed");
  }
}

int ObFTSTokenDocCache::add_token_doc_id(const ObString &token, int64_t doc_id)
{
  int ret = OB_SUCCESS;
  
  if (OB_FAIL(ensure_inited())) {
    LOG_WARN("fail to ensure inited", K(ret));
  } else if (OB_UNLIKELY(token.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // 获取全局写锁
    SpinWLockGuard guard(lock_);
    
    // 首先尝试使用 atomic_refactored 原子更新已存在的条目
    // 这样不会改变 key，只更新 value
    ObTokenDocRange updated_range;
    UpdateTokenDocRangeCallback callback(doc_id, &updated_range);
    int tmp_ret = token_doc_map_.atomic_refactored(token, callback);
    
    if (OB_SUCCESS == tmp_ret) {
      // 更新成功
      LOG_INFO("[FTS_CACHE_ADD] token updated", K(token), K(doc_id),
               "min_doc_id", updated_range.min_doc_id_,
               "max_doc_id", updated_range.max_doc_id_,
               "doc_count", updated_range.doc_count_,
               "map_size", token_doc_map_.size());
    } else if (OB_HASH_NOT_EXIST == tmp_ret) {
      // token 不存在，创建新条目
      // 需要分配内存存储 token 字符串
      ObString stored_token;
      char *buf = nullptr;
      
      if (OB_ISNULL(buf = static_cast<char*>(allocator_.alloc(token.length())))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to allocate memory for token", K(ret), K(token.length()));
      } else {
        MEMCPY(buf, token.ptr(), token.length());
        stored_token.assign_ptr(buf, token.length());
        
        ObTokenDocRange new_range;
        new_range.min_doc_id_ = doc_id;
        new_range.max_doc_id_ = doc_id;
        new_range.doc_count_ = 1;
        
        // 使用 flag=0 表示不覆盖，如果已存在则返回错误
        // 这里不应该发生，因为我们刚刚检查过不存在
        if (OB_FAIL(token_doc_map_.set_refactored(stored_token, new_range, 0/*no overwrite*/))) {
          LOG_WARN("fail to insert token doc range", K(ret), K(token));
          // 释放分配的内存（虽然 ObArenaAllocator 不支持单独释放）
        } else {
          LOG_INFO("[FTS_CACHE_ADD] token inserted", K(token), K(doc_id),
                   "min_doc_id", new_range.min_doc_id_,
                   "max_doc_id", new_range.max_doc_id_,
                   "doc_count", new_range.doc_count_,
                   "map_size", token_doc_map_.size());
        }
      }
    } else {
      ret = tmp_ret;
      LOG_WARN("fail to atomic update token", K(ret), K(token));
    }
  }

  return ret;
}

int ObFTSTokenDocCache::add_token_doc_id(const ObString &token, const ObDatum &doc_id_datum)
{
  int ret = OB_SUCCESS;

  if (doc_id_datum.is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int64_t doc_id = 0;
    const uint32_t len = doc_id_datum.len_;

    if (len == sizeof(int64_t)) {
      doc_id = doc_id_datum.get_int();
    } else if (len == sizeof(int32_t)) {
      doc_id = static_cast<int64_t>(doc_id_datum.get_int32());
    } else if (len > 0) {
      const ObString &str = doc_id_datum.get_string();
      doc_id = static_cast<int64_t>(str.hash());
    } else {
      ret = OB_INVALID_ARGUMENT;
    }

    if (OB_SUCC(ret)) {
      ret = add_token_doc_id(token, doc_id);
    }
  }

  return ret;
}

int ObFTSTokenDocCache::get_token_doc_range(const ObString &token, ObTokenDocRange &doc_range) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(token.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // 获取全局读锁
    SpinRLockGuard guard(lock_);
    
    ret = token_doc_map_.get_refactored(token, doc_range);
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_ENTRY_NOT_EXIST;
    }
  }

  return ret;
}

bool ObFTSTokenDocCache::contains_token(const ObString &token) const
{
  ObTokenDocRange tmp;
  return OB_SUCCESS == get_token_doc_range(token, tmp);
}

int64_t ObFTSTokenDocCache::get_token_count() const
{
  if (!is_inited_) {
    return 0;
  }
  
  SpinRLockGuard guard(lock_);
  return token_doc_map_.size();
}

void ObFTSTokenDocCache::print_statistics() const
{
  if (!is_inited_) {
    LOG_INFO("ObFTSTokenDocCache is not initialized");
    return;
  }

  SpinRLockGuard guard(lock_);
  LOG_INFO("ObFTSTokenDocCache statistics",
           "token_count", token_doc_map_.size(),
           "mode", "single-lock");
}

int ObFTSTokenDocCache::get_all_tokens(ObIArray<ObString> &tokens) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    tokens.reset();
    SpinRLockGuard guard(lock_);
    
    for (TokenDocMap::const_iterator it = token_doc_map_.begin();
         OB_SUCC(ret) && it != token_doc_map_.end();
         ++it) {
      if (OB_FAIL(tokens.push_back(it->first))) {
        LOG_WARN("fail to push back token", K(ret));
      }
    }
  }

  return ret;
}

} // end namespace sql
} // end namespace oceanbase
