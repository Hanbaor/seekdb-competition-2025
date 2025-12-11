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
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include <climits>

namespace oceanbase
{
namespace sql
{

// 使用 ObArray + ArenaAllocator 实现动态 base_id 列表（无大小限制）
struct ObFtsFilterCtx
{
  // id 过滤条件 (例如 id < 1000)
  int64_t id_limit_;
  bool has_id_filter_;
  
  // base_id IN 过滤条件（动态数组）
  common::ObArray<common::ObString> base_id_list_;
  bool has_base_id_filter_;
  
  // LIMIT 条件 (例如 LIMIT 10)
  int64_t limit_count_;
  bool has_limit_;
  
  // Arena 分配器（用于字符串深拷贝，reset 时一次性释放）
  common::ObArenaAllocator allocator_;
  
  // 默认构造函数
  ObFtsFilterCtx()
    : id_limit_(INT64_MAX),
      has_id_filter_(false),
      base_id_list_(),
      has_base_id_filter_(false),
      limit_count_(0),
      has_limit_(false),
      allocator_("FtsFilterCtx")
  {
  }
  
  int add_base_id(const common::ObString &base_id)
  {
    int ret = common::OB_SUCCESS;
    // 使用 arena allocator 动态分配内存
    char *buf = static_cast<char*>(allocator_.alloc(base_id.length()));
    if (OB_ISNULL(buf)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // 深拷贝字符串数据
      MEMCPY(buf, base_id.ptr(), base_id.length());
      common::ObString copied_str(static_cast<int32_t>(base_id.length()), buf);
      if (OB_FAIL(base_id_list_.push_back(copied_str))) {
        // 失败时不需要手动释放，reset() 时会一起释放
      } else {
        has_base_id_filter_ = true;
      }
    }
    return ret;
  }
  
  void reset()
  {
    id_limit_ = INT64_MAX;
    has_id_filter_ = false;
    base_id_list_.reuse();  // 清空数组但保留已分配的数组内存
    has_base_id_filter_ = false;
    limit_count_ = 0;
    has_limit_ = false;
    allocator_.reset();     // 释放所有字符串内存
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
    for (int64_t i = 0; i < base_id_list_.count(); ++i) {
      if (base_id_list_.at(i) == base_id) {
        return true;
      }
    }
    return false;
  }
  
  // 获取 base_id 数量
  int64_t base_id_count() const { return base_id_list_.count(); }
  
  TO_STRING_KV(K_(id_limit), K_(has_id_filter), "base_id_count", base_id_list_.count(), K_(has_base_id_filter));
};

// 线程局部变量，存储当前查询的过滤条件
extern thread_local ObFtsFilterCtx g_fts_filter_ctx;

} // namespace sql
} // namespace oceanbase

#endif // OB_DAS_FTS_FILTER_CTX_H_
