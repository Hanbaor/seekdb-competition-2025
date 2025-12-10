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

// POD 结构体，TLS 初始化极快（仅内存清零）
struct ObFtsFilterCtx
{
  static constexpr int64_t MAX_BASE_IDS = 16;  // 固定大小，避免动态分配
  
  // id 过滤条件 (例如 id < 1000)
  int64_t id_limit_;
  bool has_id_filter_;
  
  // base_id IN 过滤条件（固定数组，POD）
  common::ObString base_id_list_[MAX_BASE_IDS];
  int64_t base_id_count_;
  bool has_base_id_filter_;
  
  // LIMIT 条件 (例如 LIMIT 10)
  int64_t limit_count_;
  bool has_limit_;
  
  // POD：不需要构造函数，使用 reset() 初始化
  
  int add_base_id(const common::ObString &base_id)
  {
    if (base_id_count_ >= MAX_BASE_IDS) {
      return common::OB_SIZE_OVERFLOW;
    }
    base_id_list_[base_id_count_++] = base_id;
    has_base_id_filter_ = true;
    return common::OB_SUCCESS;
  }
  
  void reset()
  {
    id_limit_ = INT64_MAX;
    has_id_filter_ = false;
    base_id_count_ = 0;
    has_base_id_filter_ = false;
  }
  
  // 检查 id 是否满足条件
  bool check_id(int64_t id) const
  {
    if (!has_id_filter_) return true;
    return id < id_limit_;
  }
  
  // 检查 base_id 是否满足条件（简单遍历）
  bool check_base_id(const common::ObString &base_id) const
  {
    if (!has_base_id_filter_) return true;
    for (int64_t i = 0; i < base_id_count_; ++i) {
      if (base_id_list_[i] == base_id) {
        return true;
      }
    }
    return false;
  }
  
  TO_STRING_KV(K_(id_limit), K_(has_id_filter), K_(base_id_count), K_(has_base_id_filter));
};

// 线程局部变量，存储当前查询的过滤条件
extern thread_local ObFtsFilterCtx g_fts_filter_ctx;

} // namespace sql
} // namespace oceanbase

#endif // OB_DAS_FTS_FILTER_CTX_H_
