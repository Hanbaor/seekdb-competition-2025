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
#include "storage/tmp_file/ob_tmp_file_manager.h"
#include "storage/tmp_file/ob_tmp_file_io_info.h"
#include "storage/tmp_file/ob_tmp_file_io_handle.h"

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

// 全文检索标量字段缓存
// 存储：全落盘到 tmpfile
// 查询：先调用 load_to_memory() 加载到内存，然后 O(1) 访问
class ObFtsScalarCache
{
public:
  static const int64_t DEFAULT_CAPACITY = 200000;
  static const int64_t MAX_BASE_ID_LEN = 256;
  static const int64_t MAX_DOCID_COL_LEN = 256;
  // 固定记录大小
  static const int64_t RECORD_SIZE = 8 + 4 + MAX_BASE_ID_LEN + 4 + MAX_DOCID_COL_LEN;
  
  ObFtsScalarCache();
  ~ObFtsScalarCache();
  
  // 初始化
  int init(int64_t capacity = DEFAULT_CAPACITY);
  
  // 添加映射 - 写入 tmpfile（索引构建阶段调用）
  int add_mapping(uint64_t doc_id, 
                  uint64_t id, 
                  const common::ObString &base_id,
                  const common::ObString &docid_col);
  
  // 从 tmpfile 加载到内存（查询前调用一次）
  int load_to_memory();
  
  // 是否已加载到内存
  bool is_loaded() const { return is_loaded_; }
  
  // 查询标量字段，O(1)（需先调用 load_to_memory）
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const;
  
  // 直接获取 base_id，O(1)，返回引用，零拷贝
  const common::ObString& get_base_id_fast(uint32_t doc_id) const
  {
    static common::ObString empty_str;
    if (is_loaded_ && doc_id > 0 && doc_id <= count_) {
      return base_ids_[doc_id - 1];
    }
    return empty_str;
  }
  
  // 直接获取 id，O(1)
  uint64_t get_id_fast(uint32_t doc_id) const
  {
    if (is_loaded_ && doc_id > 0 && doc_id <= count_) {
      return ids_[doc_id - 1];
    }
    return 0;
  }
  
  // 遍历所有条目
  template<typename Fn>
  int foreach(Fn fn) const
  {
    if (!is_loaded_) return common::OB_NOT_INIT;
    ScalarFields fields;
    for (uint64_t i = 0; i < count_; ++i) {
      fields.id_ = ids_[i];
      fields.base_id_ = base_ids_[i];
      fields.docid_col_ = docid_cols_[i];
      fn(i + 1, fields);
    }
    return common::OB_SUCCESS;
  }
  
  bool is_inited() const { return is_inited_; }
  uint64_t size() const { return count_; }
  uint64_t file_record_count() const { return file_record_count_; }
  uint64_t capacity() const { return file_record_count_; }
  void destroy();
  void reuse();
  
  TO_STRING_KV(K_(is_inited), K_(is_loaded), K_(count), K_(file_record_count));
  
private:
  int ensure_tmpfile_open_();
  int write_record_(uint64_t doc_id, uint64_t id,
                    const common::ObString &base_id,
                    const common::ObString &docid_col);
  
  bool is_inited_;
  bool is_loaded_;                   // 是否已加载到内存
  lib::ObMemAttr mem_attr_;
  
  // TmpFile 相关（存储）
  uint64_t tmpfile_tenant_id_;       // 打开 tmpfile 时的租户 ID
  int64_t dir_id_;
  int64_t fd_;
  uint64_t file_record_count_;       // tmpfile 中的记录数
  
  // 内存数组（查询时使用）- 三列分离，支持返回引用
  uint64_t *ids_;                    // id 数组
  common::ObString *base_ids_;       // base_id 数组（ObString 指向 string_pool_）
  common::ObString *docid_cols_;     // docid_col 数组（ObString 指向 string_pool_）
  char *string_pool_;                // 所有字符串连续存储
  int64_t string_pool_size_;
  uint64_t count_;                   // 内存中的条目数
  
  mutable common::ObSpinLock lock_;
};

// 全局单例
extern ObFtsScalarCache g_fts_scalar_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_SCALAR_CACHE_H_
