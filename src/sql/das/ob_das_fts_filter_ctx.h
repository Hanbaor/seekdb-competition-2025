/*
 * Copyright (c) 2025 OceanBase.
 * 全文检索过滤条件上下文
 * 用于在 SQL 解析/执行阶段捕获过滤条件，传递给 BM25S 搜索使用
 */

#ifndef OB_DAS_FTS_FILTER_CTX_H_
#define OB_DAS_FTS_FILTER_CTX_H_

#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/container/ob_array.h"
#include <climits>

namespace oceanbase
{
namespace sql
{

struct ObFtsFilterCtx
{
  // id 过滤条件 (例如 id < 1000)
  int64_t id_limit_;
  bool has_id_filter_;
  
  // base_id IN 过滤条件（使用 ObArray 自动扩容）
  common::ObArray<common::ObString> base_id_list_;
  bool has_base_id_filter_;
  
  ObFtsFilterCtx()
    : id_limit_(INT64_MAX),
      has_id_filter_(false),
      base_id_list_(),
      has_base_id_filter_(false)
  {}
  
  ~ObFtsFilterCtx() {}
  
  int add_base_id(const common::ObString &base_id)
  {
    int ret = base_id_list_.push_back(base_id);
    if (common::OB_SUCCESS == ret) {
      has_base_id_filter_ = true;
    }
    return ret;
  }
  
  void reset()
  {
    id_limit_ = INT64_MAX;
    has_id_filter_ = false;
    base_id_list_.reuse();  // 清空但保留容量
    has_base_id_filter_ = false;
  }
  
  // 检查 id 是否满足条件
  bool check_id(int64_t id) const
  {
    if (!has_id_filter_) return true;
    return id < id_limit_;
  }
  
  // 检查 base_id 是否满足条件（简单遍历，对于小列表比 hash 更快）
  bool check_base_id(const common::ObString &base_id) const
  {
    if (!has_base_id_filter_) return true;
    for (int64_t i = 0; i < base_id_list_.count(); ++i) {
      if (base_id_list_.at(i) == base_id) {
        return true;
      }
    }
    return false;
  }
  
  TO_STRING_KV(K_(id_limit), K_(has_id_filter), "base_id_count", base_id_list_.count(), K_(has_base_id_filter));
};

// 线程局部变量，存储当前查询的过滤条件
extern thread_local ObFtsFilterCtx g_fts_filter_ctx;

} // namespace sql
} // namespace oceanbase

#endif // OB_DAS_FTS_FILTER_CTX_H_
