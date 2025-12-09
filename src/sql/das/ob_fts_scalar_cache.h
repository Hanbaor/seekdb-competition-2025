/*
 * Copyright (c) 2025 OceanBase.
 */

#ifndef OB_FTS_SCALAR_CACHE_H_
#define OB_FTS_SCALAR_CACHE_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/ob_mod_define.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/oblog/ob_log_module.h"
#include <cstring>

namespace oceanbase
{
namespace sql
{

// 标量字段结构体
struct ScalarFields
{
  uint64_t id_;                      // 主表的 id 字段
  common::ObString base_id_;         // 主表的 base_id 字段
  common::ObString docid_col_;       // 主表的 docid_col 字段
  
  ScalarFields() : id_(0), base_id_(), docid_col_() {}
  
  TO_STRING_KV(K_(id), K_(base_id), K_(docid_col));
};

// 全文检索标量字段缓存（动态数组实现，O(1) 访问，支持扩容）
class ObFtsScalarCache
{
public:
  static constexpr int64_t DEFAULT_CAPACITY = 2000;
  
  ObFtsScalarCache();
  ~ObFtsScalarCache();
  
  // 初始化
  int init(int64_t capacity = DEFAULT_CAPACITY);
  
  // 添加映射 (doc_id → 标量字段)，O(1)，超出容量时自动扩容
  int add_mapping(uint64_t doc_id, 
                  uint64_t id, 
                  const common::ObString &base_id,
                  const common::ObString &docid_col);
  
  // 查询标量字段，O(1)
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const;
  
  // 直接获取 base_id，O(1) 无任何开销
  const common::ObString& get_base_id_fast(uint32_t doc_id) const
  {
    static common::ObString empty_str;
    if (doc_id < capacity_ && valid_[doc_id]) {
      return data_[doc_id].base_id_;
    }
    return empty_str;
  }
  
  // 直接获取 id，O(1)
  uint64_t get_id_fast(uint32_t doc_id) const
  {
    if (doc_id < capacity_ && valid_[doc_id]) {
      return data_[doc_id].id_;
    }
    return 0;
  }
  
  // 遍历所有有效条目
  template<typename Fn>
  int foreach(Fn fn) const
  {
    if (!is_inited_) return common::OB_NOT_INIT;
    for (int64_t i = 0; i < capacity_; ++i) {
      if (valid_[i]) {
        fn(static_cast<uint64_t>(i), data_[i]);
      }
    }
    return common::OB_SUCCESS;
  }
  
  bool is_inited() const { return is_inited_; }
  int64_t size() const { return count_; }
  int64_t capacity() const { return capacity_; }
  void destroy();
  
  TO_STRING_KV(K_(is_inited), K_(count), K_(capacity));
  
private:
  // 扩容到能容纳 doc_id 的大小
  int ensure_capacity(uint64_t doc_id);
  
  bool is_inited_;
  int64_t count_;
  int64_t capacity_;
  lib::ObMemAttr mem_attr_;
  
  // 动态数组：doc_id 直接作为索引
  ScalarFields *data_;
  bool *valid_;
};

// 全局单例
extern ObFtsScalarCache g_fts_scalar_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_SCALAR_CACHE_H_
