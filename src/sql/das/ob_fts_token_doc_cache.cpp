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

// 静态常量定义（ODR-use 需要）
const int64_t ObFTSThreadLocalCache::DEFAULT_BUCKET_COUNT;
const int64_t ObFTSTokenDocCache::MAX_THREAD_CACHES;

// ==================== ObFTSThreadLocalCache ====================

ObFTSThreadLocalCache::ObFTSThreadLocalCache()
  : is_inited_(false),
    token_doc_map_(),
    allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "FTSTLCache"))
{
}

ObFTSThreadLocalCache::~ObFTSThreadLocalCache()
{
  destroy();
}

int ObFTSThreadLocalCache::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    // 已初始化
  } else if (OB_FAIL(token_doc_map_.create(DEFAULT_BUCKET_COUNT,
                                            ObMemAttr(OB_SERVER_TENANT_ID, "FTSTLMap")))) {
    LOG_WARN("fail to create thread local token doc map", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

void ObFTSThreadLocalCache::reset()
{
  if (is_inited_) {
    token_doc_map_.reuse();
    allocator_.reset();
  }
}

void ObFTSThreadLocalCache::destroy()
{
  if (is_inited_) {
    token_doc_map_.destroy();
    allocator_.reset();
    is_inited_ = false;
  }
}

int ObFTSThreadLocalCache::add_token_doc_id(const ObString &token, int64_t doc_id)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(token.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObTokenDocRange existing_range;
    int tmp_ret = token_doc_map_.get_refactored(token, existing_range);

    if (OB_SUCCESS == tmp_ret) {
      existing_range.update(doc_id);
      if (OB_FAIL(token_doc_map_.set_refactored(token, existing_range, 1/*overwrite*/))) {
        LOG_WARN("fail to update token doc range", K(ret), K(token));
      }
    } else if (OB_HASH_NOT_EXIST == tmp_ret) {
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

        if (OB_FAIL(token_doc_map_.set_refactored(stored_token, new_range))) {
          LOG_WARN("fail to insert token doc range", K(ret), K(token));
        }
      }
    } else {
      ret = tmp_ret;
    }
  }

  return ret;
}

int ObFTSThreadLocalCache::get_token_doc_range(const ObString &token, ObTokenDocRange &doc_range) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(token.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ret = token_doc_map_.get_refactored(token, doc_range);
    if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_ENTRY_NOT_EXIST;
    }
  }
  return ret;
}

// ==================== ObFTSTokenDocCache ====================

// 线程局部缓存指针（每个线程一个）
static thread_local ObFTSThreadLocalCache *tl_cache_ptr = nullptr;

ObFTSTokenDocCache::ObFTSTokenDocCache()
  : is_inited_(false),
    thread_caches_(),
    thread_cache_count_(0),
    registry_lock_()
{
  for (int64_t i = 0; i < MAX_THREAD_CACHES; ++i) {
    thread_caches_[i] = nullptr;
  }
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
    SpinWLockGuard guard(registry_lock_);
    if (!is_inited_) {
      is_inited_ = true;
      LOG_INFO("ObFTSTokenDocCache initialized (thread-local mode, lock-free write)");
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
  SpinWLockGuard guard(registry_lock_);
  // 重置所有线程局部缓存
  for (int64_t i = 0; i < thread_cache_count_; ++i) {
    if (OB_NOT_NULL(thread_caches_[i])) {
      thread_caches_[i]->reset();
    }
  }
  LOG_INFO("ObFTSTokenDocCache reset", K_(thread_cache_count));
}

void ObFTSTokenDocCache::destroy()
{
  SpinWLockGuard guard(registry_lock_);
  // 销毁所有线程局部缓存
  for (int64_t i = 0; i < thread_cache_count_; ++i) {
    if (OB_NOT_NULL(thread_caches_[i])) {
      thread_caches_[i]->destroy();
      ob_free(thread_caches_[i]);
      thread_caches_[i] = nullptr;
    }
  }
  thread_cache_count_ = 0;
  is_inited_ = false;
  LOG_INFO("ObFTSTokenDocCache destroyed");
}

int ObFTSTokenDocCache::register_thread_local_cache(ObFTSThreadLocalCache *cache)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(registry_lock_);
  
  if (thread_cache_count_ >= MAX_THREAD_CACHES) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too many thread local caches", K(ret), K_(thread_cache_count), K(MAX_THREAD_CACHES));
  } else {
    thread_caches_[thread_cache_count_++] = cache;
  }
  
  return ret;
}

ObFTSThreadLocalCache *ObFTSTokenDocCache::get_or_create_thread_local_cache()
{
  int ret = OB_SUCCESS;
  
  // 快速路径：已有线程局部缓存
  if (OB_NOT_NULL(tl_cache_ptr) && tl_cache_ptr->is_inited()) {
    return tl_cache_ptr;
  }
  
  // 慢速路径：创建新的线程局部缓存
  void *buf = ob_malloc(sizeof(ObFTSThreadLocalCache), ObMemAttr(OB_SERVER_TENANT_ID, "FTSTLCache"));
  if (OB_ISNULL(buf)) {
    LOG_WARN("fail to allocate thread local cache");
    return nullptr;
  }
  
  ObFTSThreadLocalCache *cache = new (buf) ObFTSThreadLocalCache();
  if (OB_FAIL(cache->init())) {
    LOG_WARN("fail to init thread local cache", K(ret));
    cache->~ObFTSThreadLocalCache();
    ob_free(buf);
    return nullptr;
  }
  
  // 注册到全局列表
  if (OB_FAIL(register_thread_local_cache(cache))) {
    LOG_WARN("fail to register thread local cache", K(ret));
    cache->destroy();
    cache->~ObFTSThreadLocalCache();
    ob_free(buf);
    return nullptr;
  }
  
  tl_cache_ptr = cache;
  return cache;
}

int ObFTSTokenDocCache::add_token_doc_id(const ObString &token, int64_t doc_id)
{
  int ret = OB_SUCCESS;
  
  if (OB_FAIL(ensure_inited())) {
    LOG_WARN("fail to ensure inited", K(ret));
  } else {
    // 获取或创建线程局部缓存
    ObFTSThreadLocalCache *cache = get_or_create_thread_local_cache();
    if (OB_ISNULL(cache)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // 直接写入线程局部缓存，无锁！
      ret = cache->add_token_doc_id(token, doc_id);
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
  int ret = OB_ENTRY_NOT_EXIST;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(token.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // 遍历所有线程局部缓存，合并结果
    SpinRLockGuard guard(registry_lock_);
    
    ObTokenDocRange merged_range;
    bool found = false;
    
    for (int64_t i = 0; i < thread_cache_count_; ++i) {
      if (OB_NOT_NULL(thread_caches_[i]) && thread_caches_[i]->is_inited()) {
        ObTokenDocRange local_range;
        int tmp_ret = thread_caches_[i]->get_token_doc_range(token, local_range);
        if (OB_SUCCESS == tmp_ret) {
          if (!found) {
            merged_range = local_range;
            found = true;
          } else {
            merged_range.merge(local_range);
          }
        }
      }
    }
    
    if (found) {
      doc_range = merged_range;
      ret = OB_SUCCESS;
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
  
  int64_t total = 0;
  SpinRLockGuard guard(registry_lock_);
  
  for (int64_t i = 0; i < thread_cache_count_; ++i) {
    if (OB_NOT_NULL(thread_caches_[i]) && thread_caches_[i]->is_inited()) {
      total += thread_caches_[i]->get_map().size();
    }
  }
  
  return total;
}

void ObFTSTokenDocCache::print_statistics() const
{
  if (!is_inited_) {
    LOG_INFO("ObFTSTokenDocCache is not initialized");
    return;
  }

  SpinRLockGuard guard(registry_lock_);
  
  int64_t total_tokens = 0;
  for (int64_t i = 0; i < thread_cache_count_; ++i) {
    if (OB_NOT_NULL(thread_caches_[i]) && thread_caches_[i]->is_inited()) {
      total_tokens += thread_caches_[i]->get_map().size();
    }
  }
  
  LOG_INFO("ObFTSTokenDocCache statistics",
           K(total_tokens),
           K_(thread_cache_count),
           "mode", "thread-local-lock-free");
}

int ObFTSTokenDocCache::get_all_tokens(ObIArray<ObString> &tokens) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    tokens.reset();
    SpinRLockGuard guard(registry_lock_);
    
    // 使用临时 set 去重
    common::hash::ObHashSet<ObString> token_set;
    if (OB_FAIL(token_set.create(100000))) {
      LOG_WARN("fail to create token set", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < thread_cache_count_; ++i) {
        if (OB_NOT_NULL(thread_caches_[i]) && thread_caches_[i]->is_inited()) {
          const TokenDocMap &local_map = thread_caches_[i]->get_map();
          for (TokenDocMap::const_iterator it = local_map.begin();
               OB_SUCC(ret) && it != local_map.end();
               ++it) {
            if (OB_FAIL(token_set.set_refactored(it->first))) {
              if (OB_HASH_EXIST == ret) {
                ret = OB_SUCCESS; // 重复的 token，忽略
              }
            }
          }
        }
      }
      
      // 转换为数组
      for (common::hash::ObHashSet<ObString>::const_iterator it = token_set.begin();
           OB_SUCC(ret) && it != token_set.end();
           ++it) {
        if (OB_FAIL(tokens.push_back(it->first))) {
          LOG_WARN("fail to push back token", K(ret));
        }
      }
    }
  }

  return ret;
}

} // end namespace sql
} // end namespace oceanbase
