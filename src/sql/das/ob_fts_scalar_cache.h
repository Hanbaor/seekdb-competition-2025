/*
 * Copyright (c) 2025 OceanBase.
 */

#ifndef OB_FTS_SCALAR_CACHE_H_
#define OB_FTS_SCALAR_CACHE_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/lock/ob_spin_lock.h"

namespace oceanbase
{
namespace sql
{

// 标量字段结构体（用于返回结果）
struct ScalarFields
{
  uint64_t id_;                      // 主表的 id 字段
  common::ObString base_id_;         // 主表的 base_id 字段
  common::ObString docid_col_;       // 主表的 docid_col 字段
  
  ScalarFields() : id_(0), base_id_(), docid_col_() {}
  
  TO_STRING_KV(K_(id), K_(base_id), K_(docid_col));
};

// 全文检索标量字段缓存（列式存储，分散内存分配）
class ObFtsScalarCache
{
public:
  static const int64_t DEFAULT_CAPACITY = 200000;
  
  ObFtsScalarCache();
  ~ObFtsScalarCache();
  
  // 初始化（预分配容量）
  int init(int64_t capacity = DEFAULT_CAPACITY);
  
  // 添加映射 (doc_id → 标量字段)，doc_id 从 1 开始
  int add_mapping(uint64_t doc_id, 
                  uint64_t id, 
                  const common::ObString &base_id,
                  const common::ObString &docid_col);
  
  // 查询标量字段，O(1)
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const;
  
  // 直接获取 base_id，O(1)，doc_id 从 1 开始
  common::ObString get_base_id_fast(uint32_t doc_id) const
  {
    common::ObSpinLockGuard guard(lock_);
    if (doc_id > 0 && doc_id <= count_) {
      return base_ids_[doc_id - 1];
    }
    return common::ObString();
  }
  
  // 直接获取 id，O(1)，doc_id 从 1 开始
  uint64_t get_id_fast(uint32_t doc_id) const
  {
    common::ObSpinLockGuard guard(lock_);
    if (doc_id > 0 && doc_id <= count_) {
      return ids_[doc_id - 1];
    }
    return 0;
  }
  
  // 遍历所有条目，doc_id 从 1 开始
  template<typename Fn>
  int foreach(Fn fn) const
  {
    common::ObSpinLockGuard guard(lock_);
    if (!is_inited_) return common::OB_NOT_INIT;
    ScalarFields fields;
    for (uint64_t i = 0; i < count_; ++i) {
      fields.id_ = ids_[i];
      fields.base_id_ = base_ids_[i];
      fields.docid_col_ = docid_cols_[i];
      fn(i + 1, fields);  // doc_id = i + 1
    }
    return common::OB_SUCCESS;
  }
  
  bool is_inited() const { return is_inited_; }
  uint64_t size() const { return count_; }
  int64_t capacity() const { return capacity_; }
  void destroy();
  void reuse();
  
  TO_STRING_KV(K_(is_inited), K_(count), K_(capacity));
  
private:
  int ensure_capacity(uint64_t required);
  
  bool is_inited_;
  lib::ObMemAttr mem_attr_;
  
  // 三个独立数组
  uint64_t *ids_;                    // id 列
  common::ObString *base_ids_;       // base_id 列
  common::ObString *docid_cols_;     // docid_col 列
  
  uint64_t count_;                   // 当前条目数
  int64_t capacity_;                 // 容量
  mutable common::ObSpinLock lock_;  // 多线程保护
};

// 全局单例
extern ObFtsScalarCache g_fts_scalar_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_SCALAR_CACHE_H_
