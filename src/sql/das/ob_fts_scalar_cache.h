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

// 全文检索标量字段缓存（纯内存版，支持乱序写入）
class ObFtsScalarCache
{
public:
  static const int64_t DEFAULT_CAPACITY = 200000;
  
  ObFtsScalarCache();
  ~ObFtsScalarCache();
  
  // 初始化
  int init(int64_t capacity = DEFAULT_CAPACITY);
  
  // 添加映射 (线程安全)
  int add_mapping(uint64_t doc_id, 
                  uint64_t id, 
                  const common::ObString &base_id,
                  const common::ObString &docid_col);
  
  // 这里的 load_to_memory 对于纯内存版其实是 no-op，但为了兼容接口保留
  int load_to_memory() { return common::OB_SUCCESS; }
  bool is_loaded() const { return is_inited_; }
  bool is_inited() const { return is_inited_; }
  uint64_t size() const { return count_; }
  uint64_t capacity() const { return capacity_; }
  
  // O(1) 查询
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const;

  // O(1) 查询 (Fast)
  const common::ObString& get_base_id_fast(uint32_t doc_id) const {
    if (doc_id > 0 && doc_id <= count_) {
      return base_ids_[doc_id - 1];
    }
    static common::ObString empty_str;
    return empty_str;
  }
  
  uint64_t get_id_fast(uint32_t doc_id) const {
    if (doc_id > 0 && doc_id <= count_) {
      return ids_[doc_id - 1];
    }
    return 0;
  }

  void reuse();
  void destroy();

private:
  // 扩容
  int ensure_capacity(uint64_t required_size);

private:
  bool is_inited_;
  lib::ObMemAttr mem_attr_;
  
  uint64_t *ids_;
  common::ObString *base_ids_;
  common::ObString *docid_cols_;
  
  uint64_t count_;
  uint64_t capacity_;
  
  mutable common::ObSpinLock lock_;
};

// 全局单例
extern ObFtsScalarCache g_fts_scalar_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_SCALAR_CACHE_H_
