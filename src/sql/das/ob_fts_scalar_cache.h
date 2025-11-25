/*
 * Copyright (c) 2025 OceanBase.
 */

#ifndef OB_FTS_SCALAR_CACHE_H_
#define OB_FTS_SCALAR_CACHE_H_

#include "lib/hash/ob_hashmap.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{

// 标量字段结构体
struct ScalarFields
{
  uint64_t id_;              // 主表的 id 字段
  common::ObString base_id_;         // 主表的 base_id 字段
  common::ObString docid_col_;       // 主表的 docid_col 字段
  
  ScalarFields() : id_(0), base_id_(), docid_col_() {}
  
  TO_STRING_KV(K_(id), K_(base_id), K_(docid_col));
};

// 全文检索标量字段缓存
class ObFtsScalarCache
{
public:
  ObFtsScalarCache();
  ~ObFtsScalarCache();
  
  // 初始化
  int init(int64_t capacity = 1000000);
  
  // 添加映射 (doc_id → 标量字段)
  int add_mapping(uint64_t doc_id, 
                  uint64_t id, 
                  const common::ObString &base_id,
                  const common::ObString &docid_col);
  
  // 查询标量字段
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields);
  
  // 检查是否初始化
  bool is_inited() const { return is_inited_; }
  
  // 获取大小
  int64_t size() const;
  
  // 清空
  void destroy();
  
  TO_STRING_KV(K_(is_inited), "size", size());
  
private:
  bool is_inited_;
  common::ObArenaAllocator allocator_;
  common::hash::ObHashMap<uint64_t, ScalarFields> mapping_;
};

// 全局单例
extern ObFtsScalarCache g_fts_scalar_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_SCALAR_CACHE_H_
