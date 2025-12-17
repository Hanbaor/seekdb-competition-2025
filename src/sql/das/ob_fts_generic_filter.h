/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用过滤上下文 - 支持任意列的任意类型过滤
 */

#ifndef OB_FTS_GENERIC_FILTER_H_
#define OB_FTS_GENERIC_FILTER_H_

#include "lib/container/ob_array.h"
#include "lib/allocator/page_arena.h"
#include "lib/oblog/ob_log_module.h"
#include "share/cache/ob_kv_storecache.h"
#include "sql/das/ob_fts_column_value.h"
#include "sql/das/ob_fts_generic_cache.h"
#include "sql/das/ob_fts_scalar_kvcache.h"
#include "sql/engine/expr/ob_expr.h"

namespace oceanbase
{
namespace sql
{

/**
 * 支持的过滤操作类型
 */
enum ObFtsFilterOpType
{
  FTS_OP_NONE = 0,

  // 比较操作
  FTS_OP_LT,        // <
  FTS_OP_LE,        // <=
  FTS_OP_GT,        // >
  FTS_OP_GE,        // >=
  FTS_OP_EQ,        // =
  FTS_OP_NE,        // != / <>

  // 集合操作
  FTS_OP_IN,        // IN (...)
  FTS_OP_NOT_IN,    // NOT IN (...)

  // 字符串操作
  FTS_OP_LIKE,      // LIKE
  FTS_OP_NOT_LIKE,  // NOT LIKE

  // NULL 检查
  FTS_OP_IS_NULL,   // IS NULL
  FTS_OP_IS_NOT_NULL, // IS NOT NULL

  // 范围操作
  FTS_OP_BETWEEN,   // BETWEEN ... AND ...
};

// 获取操作类型名称（用于日志）
inline const char *get_filter_op_name(ObFtsFilterOpType op)
{
  switch (op) {
    case FTS_OP_NONE: return "NONE";
    case FTS_OP_LT: return "LT";
    case FTS_OP_LE: return "LE";
    case FTS_OP_GT: return "GT";
    case FTS_OP_GE: return "GE";
    case FTS_OP_EQ: return "EQ";
    case FTS_OP_NE: return "NE";
    case FTS_OP_IN: return "IN";
    case FTS_OP_NOT_IN: return "NOT_IN";
    case FTS_OP_LIKE: return "LIKE";
    case FTS_OP_NOT_LIKE: return "NOT_LIKE";
    case FTS_OP_IS_NULL: return "IS_NULL";
    case FTS_OP_IS_NOT_NULL: return "IS_NOT_NULL";
    case FTS_OP_BETWEEN: return "BETWEEN";
    default: return "UNKNOWN";
  }
}

/**
 * 单个过滤条件
 */
struct ObFtsFilterCondition
{
  int64_t column_idx_;                    // 在缓存中的列索引
  ObFtsFilterOpType op_type_;             // 操作类型
  common::ObArray<ObFtsColumnValue> values_; // 比较值（IN 可以有多个）

  ObFtsFilterCondition()
    : column_idx_(-1),
      op_type_(FTS_OP_NONE)
  {}

  void reset()
  {
    column_idx_ = -1;
    op_type_ = FTS_OP_NONE;
    values_.reset();
  }

  /**
   * 检查单个值是否满足条件（内联以减少函数调用开销）
   */
  inline bool check(const ObFtsColumnValue &col_value) const
  {
    const int64_t val_count = values_.count();
    if (op_type_ == FTS_OP_NONE) {
      return true;  // 无效条件，默认通过
    }

    switch (op_type_) {
      case FTS_OP_LT:
        return val_count > 0 && col_value < values_.at(0);
      case FTS_OP_LE:
        return val_count > 0 && col_value <= values_.at(0);
      case FTS_OP_GT:
        return val_count > 0 && col_value > values_.at(0);
      case FTS_OP_GE:
        return val_count > 0 && col_value >= values_.at(0);
      case FTS_OP_EQ:
        return val_count > 0 && col_value == values_.at(0);
      case FTS_OP_NE:
        return val_count > 0 && col_value != values_.at(0);
      case FTS_OP_IN: {
        for (int64_t i = 0; i < val_count; ++i) {
          if (col_value == values_.at(i)) {
            return true;
          }
        }
        return false;
      }
      case FTS_OP_NOT_IN: {
        for (int64_t i = 0; i < val_count; ++i) {
          if (col_value == values_.at(i)) {
            return false;
          }
        }
        return true;
      }
      case FTS_OP_IS_NULL:
        return col_value.is_null();
      case FTS_OP_IS_NOT_NULL:
        return !col_value.is_null();
      case FTS_OP_BETWEEN:
        if (val_count >= 2) {
          return col_value >= values_.at(0) && col_value <= values_.at(1);
        }
        return true;
      case FTS_OP_LIKE:
      case FTS_OP_NOT_LIKE:
        return true;  // LIKE 操作暂不实现完整语义
      default:
        return true;
    }
  }

  TO_STRING_KV(K_(column_idx), "op", get_filter_op_name(op_type_), "value_count", values_.count());
};

/**
 * 通用过滤上下文
 *
 * 设计特点：
 * 1. 支持任意列的任意类型过滤
 * 2. 支持自动从 SQL filter 表达式提取过滤条件
 * 3. 支持多条件 AND 组合
 * 4. 线程局部存储，无锁访问
 */
class ObFtsGenericFilterCtx
{
public:
  static const int64_t MAX_CONDITIONS = 32;

  ObFtsGenericFilterCtx();
  ~ObFtsGenericFilterCtx();

  /**
   * 添加过滤条件
   * @param column_idx 在缓存中的列索引
   * @param op_type 操作类型
   * @param values 比较值数组
   */
  int add_condition(int64_t column_idx,
                    ObFtsFilterOpType op_type,
                    const common::ObIArray<ObFtsColumnValue> &values);

  /**
   * 便捷方法：添加 column < value 条件
   */
  int add_lt_condition(int64_t column_idx, const ObFtsColumnValue &value);

  /**
   * 便捷方法：添加 column IN (values) 条件
   */
  int add_in_condition(int64_t column_idx,
                       const common::ObIArray<ObFtsColumnValue> &values);

  /**
   * 从 SQL filter 表达式自动提取过滤条件
   *
   * @param filter_exprs SQL WHERE 子句中的过滤表达式
   * @param output_exprs 输出列表达式（用于匹配列）
   * @param cache 缓存对象（用于获取列元数据）
   * @param eval_ctx 表达式计算上下文
   */
  int extract_from_filters(const common::ObIArray<ObExpr*> &filter_exprs,
                           const common::ObIArray<ObExpr*> &output_exprs,
                           const ObFtsGenericCache &cache,
                           ObEvalCtx &eval_ctx);

  /**
   * 检查一行数据是否满足所有过滤条件
   */
  bool check_row(uint64_t doc_id, const ObFtsGenericCache &cache) const;

  /**
   * 快速检查（仅使用 KVCache 模式）
   * 内联实现以减少函数调用开销
   */
  inline bool check_row_fast(uint64_t doc_id, ObFtsGenericCache &cache) const
  {
    if (condition_count_ == 0) {
      return true;
    }

    if (doc_id == 0) {
      return false;
    }

    // 仅使用 KVCache 模式
    if (!cache.is_ready()) {
      return true;  // 缓存不可用，跳过过滤
    }
    
    // 检查 doc_id 范围
    uint64_t max_doc_id = static_cast<uint64_t>(cache.get_file_header().row_count);
    if (doc_id > max_doc_id) {
      return false;
    }

    // 从 KVCache 获取行数据
    const ObFtsScalarCacheValue *row = nullptr;
    common::ObKVCacheHandle handle;
    int ret = cache.get_row_from_kvcache(doc_id, row, handle);
    if (common::OB_SUCCESS != ret || row == nullptr) {
      return true;  // 获取失败，跳过过滤
    }
    
    // 检查每个条件
    const int64_t col_count = cache.column_count();
    for (int64_t i = 0; i < condition_count_; ++i) {
      const ObFtsFilterCondition &cond = conditions_[i];
      const int64_t col_idx = cond.column_idx_;
      if (col_idx < 0 || col_idx >= col_count) continue;
      
      // 从 KVCache 行数据获取列值
      ObFtsColumnValue col_val;
      if (common::OB_SUCCESS != row->get_column_value(col_idx, col_val)) {
        continue;  // 获取失败，跳过这个条件
      }
      if (!cond.check(col_val)) {
        return false;
      }
    }
    // handle 析构时自动释放 KVCache 引用
    return true;
  }

  /**
   * V2: 从 KVCache 行数据检查过滤条件
   * @param row KVCache 中的行数据
   */
  bool check_row_from_kvcache(const ObFtsScalarCacheValue *row) const;

  // ========== 状态查询 ==========
  bool has_conditions() const { return condition_count_ > 0; }
  int64_t condition_count() const { return condition_count_; }
  const ObFtsFilterCondition& get_condition(int64_t idx) const;

  /**
   * 获取指定列的范围上限（用于 scores 数组分配优化）
   * 如果该列有 LT/LE 条件，返回上限值；否则返回 INT64_MAX
   */
  int64_t get_column_upper_bound(int64_t column_idx) const;

  /**
   * 检查指定列是否有 IN 过滤条件
   */
  bool has_in_condition(int64_t column_idx) const;

  // ========== LIMIT 条件 ==========
  void set_limit(int64_t limit) { limit_count_ = limit; has_limit_ = true; }
  bool has_limit() const { return has_limit_; }
  int64_t get_limit() const { return limit_count_; }

  void reset();

private:
  /**
   * 尝试从单个表达式提取过滤条件
   */
  int try_extract_condition(ObExpr *expr,
                            const common::ObIArray<ObExpr*> &output_exprs,
                            const ObFtsGenericCache &cache,
                            ObEvalCtx &eval_ctx);

  /**
   * 查找表达式对应的缓存列索引
   */
  int find_column_index(ObExpr *expr,
                        const common::ObIArray<ObExpr*> &output_exprs,
                        const ObFtsGenericCache &cache,
                        int64_t &column_idx);

  /**
   * 提取常量值
   */
  int extract_const_value(ObExpr *expr,
                          ObEvalCtx &eval_ctx,
                          common::ObIArray<ObFtsColumnValue> &values);

  /**
   * 将表达式类型转换为过滤操作类型
   */
  static ObFtsFilterOpType convert_expr_type(ObExprOperatorType type);

  /**
   * 翻转操作符（用于 const OP column 的情况）
   */
  static ObFtsFilterOpType flip_op(ObFtsFilterOpType op);

private:
  ObFtsFilterCondition conditions_[MAX_CONDITIONS];
  int64_t condition_count_;

  int64_t limit_count_;
  bool has_limit_;

  common::ObArenaAllocator allocator_;         // 用于存储条件值（每次查询重置）
};

// 线程局部存储
extern thread_local ObFtsGenericFilterCtx g_fts_generic_filter_ctx;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_GENERIC_FILTER_H_
