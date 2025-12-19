/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用过滤上下文实现
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_generic_filter.h"
#include "share/ob_errno.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
namespace sql
{

// 线程局部存储
thread_local ObFtsGenericFilterCtx g_fts_generic_filter_ctx;

// 静态成员变量定义（链接器需要）
const int64_t ObFtsGenericFilterCtx::MAX_CONDITIONS;

// 静态空条件
static ObFtsFilterCondition EMPTY_CONDITION;

// ========== ObFtsFilterCondition::check 已移至头文件（内联）==========

// ========== ObFtsGenericFilterCtx 实现 ==========

ObFtsGenericFilterCtx::ObFtsGenericFilterCtx()
  : condition_count_(0),
    limit_count_(0),
    has_limit_(false),
    allocator_("FtsGenFilter")
{
}

ObFtsGenericFilterCtx::~ObFtsGenericFilterCtx()
{
  reset();
}

void ObFtsGenericFilterCtx::reset()
{
  for (int64_t i = 0; i < condition_count_; ++i) {
    conditions_[i].reset();
  }
  condition_count_ = 0;
  limit_count_ = 0;
  has_limit_ = false;
  
  allocator_.reset();

  //LOG_DEBUG("[FTS_GENERIC_FILTER] filter context reset");
}

int ObFtsGenericFilterCtx::add_condition(int64_t column_idx,
                                         ObFtsFilterOpType op_type,
                                         const common::ObIArray<ObFtsColumnValue> &values)
{
  int ret = common::OB_SUCCESS;

  if (condition_count_ >= MAX_CONDITIONS) {
    ret = common::OB_SIZE_OVERFLOW;
    LOG_WARN("[FTS_GENERIC_FILTER] too many conditions", K(ret), K_(condition_count));
    return ret;
  }

  if (column_idx < 0 || op_type == FTS_OP_NONE) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_FILTER] invalid condition", K(ret), K(column_idx), K(op_type));
    return ret;
  }

  ObFtsFilterCondition &cond = conditions_[condition_count_];
  cond.column_idx_ = column_idx;
  cond.op_type_ = op_type;
  cond.values_.reset();

  // 预分配空间，减少动态增长
  const int64_t val_count = values.count();
  if (val_count > 0 && OB_FAIL(cond.values_.reserve(val_count))) {
    LOG_WARN("[FTS_GENERIC_FILTER] failed to reserve values", K(ret), K(val_count));
    return ret;
  }

  // 深拷贝值
  for (int64_t i = 0; OB_SUCC(ret) && i < val_count; ++i) {
    ObFtsColumnValue copied_val;
    if (OB_FAIL(values.at(i).deep_copy(allocator_, copied_val))) {
      LOG_WARN("[FTS_GENERIC_FILTER] failed to deep copy value", K(ret), K(i));
    } else if (OB_FAIL(cond.values_.push_back(copied_val))) {
      LOG_WARN("[FTS_GENERIC_FILTER] failed to push back value", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    condition_count_++;
    // 生产环境注释掉以提升 QPS
    // if (values.count() > 0) {
    //   const ObFtsColumnValue &first_val = values.at(0);
    //   LOG_INFO("[FTS_GENERIC_FILTER] condition added",
    //            K(column_idx), "op", get_filter_op_name(op_type),
    //            "value_count", values.count(), K_(condition_count),
    //            "first_val_is_null", first_val.is_null(),
    //            "first_val_int", first_val.is_null() ? 0 : first_val.get_int(),
    //            "first_val_str", first_val.is_null() ? "" : first_val.get_string());
    // } else {
    //   LOG_INFO("[FTS_GENERIC_FILTER] condition added (no values)",
    //            K(column_idx), "op", get_filter_op_name(op_type), K_(condition_count));
    // }
  }

  return ret;
}

// 单值条件快速路径：避免创建临时 ObArray，减少内存分配
int ObFtsGenericFilterCtx::add_single_value_condition(
    int64_t column_idx,
    ObFtsFilterOpType op_type,
    const ObFtsColumnValue &value)
{
  int ret = common::OB_SUCCESS;

  if (condition_count_ >= MAX_CONDITIONS) {
    ret = common::OB_SIZE_OVERFLOW;
    LOG_WARN("[FTS_GENERIC_FILTER] too many conditions", K(ret), K_(condition_count));
    return ret;
  }

  if (column_idx < 0 || op_type == FTS_OP_NONE) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_FILTER] invalid condition", K(ret), K(column_idx), K(op_type));
    return ret;
  }

  ObFtsFilterCondition &cond = conditions_[condition_count_];
  cond.column_idx_ = column_idx;
  cond.op_type_ = op_type;
  cond.values_.reset();

  // 对于非字符串类型，直接赋值（无需 deep_copy）
  if (value.type_ != ObFtsColumnValue::TYPE_STRING) {
    if (OB_FAIL(cond.values_.push_back(value))) {
      LOG_WARN("[FTS_GENERIC_FILTER] failed to push back value", K(ret));
    }
  } else {
    // 字符串类型需要深拷贝
    ObFtsColumnValue copied_val;
    if (OB_FAIL(value.deep_copy(allocator_, copied_val))) {
      LOG_WARN("[FTS_GENERIC_FILTER] failed to deep copy value", K(ret));
    } else if (OB_FAIL(cond.values_.push_back(copied_val))) {
      LOG_WARN("[FTS_GENERIC_FILTER] failed to push back value", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    condition_count_++;
    // 临时日志：确认快速路径被调用
    /*LOG_INFO("[FTS_GENERIC_FILTER] add_single_value_condition called",
             K(column_idx), "op", get_filter_op_name(op_type),
             "value_type", value.type_, K_(condition_count));*/
  }

  return ret;
}

// 无值条件快速路径：用于 IS NULL / IS NOT NULL
int ObFtsGenericFilterCtx::add_no_value_condition(int64_t column_idx, ObFtsFilterOpType op_type)
{
  int ret = common::OB_SUCCESS;

  if (condition_count_ >= MAX_CONDITIONS) {
    ret = common::OB_SIZE_OVERFLOW;
    return ret;
  }

  if (column_idx < 0 || op_type == FTS_OP_NONE) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }

  ObFtsFilterCondition &cond = conditions_[condition_count_];
  cond.column_idx_ = column_idx;
  cond.op_type_ = op_type;
  cond.values_.reset();  // 无值条件，values_ 保持为空
  condition_count_++;

  return ret;
}

int ObFtsGenericFilterCtx::add_lt_condition(int64_t column_idx, const ObFtsColumnValue &value)
{
  // 使用快速路径
  return add_single_value_condition(column_idx, FTS_OP_LT, value);
}

int ObFtsGenericFilterCtx::add_in_condition(int64_t column_idx,
                                            const common::ObIArray<ObFtsColumnValue> &values)
{
  return add_condition(column_idx, FTS_OP_IN, values);
}

const ObFtsFilterCondition& ObFtsGenericFilterCtx::get_condition(int64_t idx) const
{
  if (idx >= 0 && idx < condition_count_) {
    return conditions_[idx];
  }
  return EMPTY_CONDITION;
}

int64_t ObFtsGenericFilterCtx::get_column_upper_bound(int64_t column_idx) const
{
  for (int64_t i = 0; i < condition_count_; ++i) {
    const ObFtsFilterCondition &cond = conditions_[i];
    if (cond.column_idx_ == column_idx) {
      if (cond.op_type_ == FTS_OP_LT || cond.op_type_ == FTS_OP_LE) {
        if (cond.values_.count() > 0) {
          const ObFtsColumnValue &val = cond.values_.at(0);
          if (val.type_ == ObFtsColumnValue::TYPE_INT64) {
            return val.get_int();
          } else if (val.type_ == ObFtsColumnValue::TYPE_UINT64) {
            return static_cast<int64_t>(val.get_uint());
          }
        }
      }
    }
  }
  return INT64_MAX;
}

bool ObFtsGenericFilterCtx::has_in_condition(int64_t column_idx) const
{
  for (int64_t i = 0; i < condition_count_; ++i) {
    if (conditions_[i].column_idx_ == column_idx &&
        conditions_[i].op_type_ == FTS_OP_IN) {
      return true;
    }
  }
  return false;
}

bool ObFtsGenericFilterCtx::check_row(uint64_t doc_id, const ObFtsGenericCache &cache) const
{
  if (condition_count_ == 0) {
    return true;  // 无过滤条件，全部通过
  }

  if (doc_id == 0 || doc_id > static_cast<uint64_t>(cache.size())) {
    return false;  // 无效的 doc_id
  }

  // 检查每个条件（AND 语义）
  for (int64_t i = 0; i < condition_count_; ++i) {
    const ObFtsFilterCondition &cond = conditions_[i];

    if (cond.column_idx_ < 0 ||
        cond.column_idx_ >= cache.column_count()) {
      continue;  // 无效的列索引，跳过
    }

    ObFtsColumnValue col_val;
    int ret = cache.get_column_value(doc_id, cond.column_idx_, col_val);
    if (common::OB_SUCCESS != ret) {
      continue;
    }

    if (!cond.check(col_val)) {
      return false;  // 条件不满足
    }
  }

  return true;  // 所有条件都满足
}

// check_row_fast 已移至头文件（内联）

bool ObFtsGenericFilterCtx::check_row_from_kvcache(const ObFtsScalarCacheValue *row) const
{
  if (condition_count_ == 0 || OB_ISNULL(row)) {
    return true;
  }
  
  // 检查每个条件
  for (int64_t i = 0; i < condition_count_; ++i) {
    const ObFtsFilterCondition &cond = conditions_[i];
    const int64_t col_idx = cond.column_idx_;
    
    if (col_idx < 0 || col_idx >= row->column_count()) {
      continue;
    }
    
    // 从缓存值中获取列数据
    ObFtsColumnValue col_val;
    if (OB_SUCCESS != row->get_column_value(col_idx, col_val)) {
      continue;
    }
    
    if (!cond.check(col_val)) {
      return false;
    }
  }
  
  return true;
}

int ObFtsGenericFilterCtx::extract_from_filters(
    const common::ObIArray<ObExpr*> &filter_exprs,
    const common::ObIArray<ObExpr*> &output_exprs,
    const ObFtsGenericCache &cache,
    ObEvalCtx &eval_ctx)
{
  int ret = common::OB_SUCCESS;

  // 生产环境注释掉以提升 QPS
  // LOG_INFO("[FTS_GENERIC_FILTER] extracting conditions from filters",
  //          "filter_count", filter_exprs.count(),
  //          "output_count", output_exprs.count(),
  //          "cache_cols", cache.column_count());

  for (int64_t i = 0; i < filter_exprs.count(); ++i) {
    ObExpr *expr = filter_exprs.at(i);
    if (OB_ISNULL(expr)) continue;

    // 递归处理 AND 表达式
    if (expr->type_ == T_OP_AND) {
      for (int64_t j = 0; j < expr->arg_cnt_; ++j) {
        int extract_ret = try_extract_condition(expr->args_[j],
                                                 output_exprs, cache, eval_ctx);
        if (common::OB_SUCCESS != extract_ret) {
          // 提取失败不是错误，只是该条件不能下推
          /*LOG_DEBUG("[FTS_GENERIC_FILTER] cannot extract sub-condition",
                    K(extract_ret), K(j));*/
        }
      }
    } else {
      int extract_ret = try_extract_condition(expr, output_exprs, cache, eval_ctx);
      if (common::OB_SUCCESS != extract_ret) {
        /*LOG_DEBUG("[FTS_GENERIC_FILTER] cannot extract condition",
                  K(extract_ret), K(i));*/
      }
    }
  }

  // 生产环境注释掉以提升 QPS
  // LOG_INFO("[FTS_GENERIC_FILTER] extraction complete",
  //          K_(condition_count));

  return ret;
}

int ObFtsGenericFilterCtx::try_extract_condition(
    ObExpr *expr,
    const common::ObIArray<ObExpr*> &output_exprs,
    const ObFtsGenericCache &cache,
    ObEvalCtx &eval_ctx)
{
  int ret = common::OB_SUCCESS;

  if (OB_ISNULL(expr) || expr->arg_cnt_ < 1) {
    return common::OB_INVALID_ARGUMENT;
  }

  ObExpr *column_expr = nullptr;
  ObExpr *value_expr = nullptr;
  int64_t column_idx = -1;
  ObFtsFilterOpType op_type = FTS_OP_NONE;
  bool flip = false;

  switch (expr->type_) {
    // ===== 比较操作 =====
    case T_OP_LT:
    case T_OP_LE:
    case T_OP_GT:
    case T_OP_GE:
    case T_OP_EQ:
    case T_OP_NE:
    case T_OP_NSEQ: {
      if (expr->arg_cnt_ != 2) {
        return common::OB_NOT_SUPPORTED;
      }

      // 检查格式：column OP const 或 const OP column
      if (expr->args_[0]->type_ == T_REF_COLUMN) {
        column_expr = expr->args_[0];
        value_expr = expr->args_[1];
        op_type = convert_expr_type(expr->type_);
      } else if (expr->args_[1]->type_ == T_REF_COLUMN) {
        // const OP column -> 翻转操作符
        column_expr = expr->args_[1];
        value_expr = expr->args_[0];
        op_type = flip_op(convert_expr_type(expr->type_));
        flip = true;
      } else {
        return common::OB_NOT_SUPPORTED;
      }
      break;
    }

    // ===== IN 操作 =====
    case T_OP_IN: {
      if (expr->arg_cnt_ < 2) {
        return common::OB_NOT_SUPPORTED;
      }
      if (expr->args_[0]->type_ != T_REF_COLUMN) {
        return common::OB_NOT_SUPPORTED;
      }

      column_expr = expr->args_[0];
      op_type = FTS_OP_IN;

      // 查找列索引
      if (OB_FAIL(find_column_index(column_expr, output_exprs, cache, column_idx))) {
        /*LOG_DEBUG("[FTS_GENERIC_FILTER] cannot find column for IN",
                  K(ret), K(column_expr->type_));*/
        return ret;
      }

      // 提取 IN 列表中的所有值
      common::ObArray<ObFtsColumnValue> values;
      values.reserve(expr->arg_cnt_ - 1);  // 预分配空间
      for (int64_t k = 1; k < expr->arg_cnt_; ++k) {
        ObExpr *val_expr = expr->args_[k];
        if (OB_ISNULL(val_expr)) continue;

        // 处理 ROW 包装
        if (val_expr->type_ == T_OP_ROW) {
          for (int64_t m = 0; m < val_expr->arg_cnt_; ++m) {
            int val_ret = extract_const_value(val_expr->args_[m], eval_ctx, values);
            if (common::OB_SUCCESS != val_ret) {
              // 忽略无法提取的值
            }
          }
        } else {
          int val_ret = extract_const_value(val_expr, eval_ctx, values);
          if (common::OB_SUCCESS != val_ret) {
            // 忽略
          }
        }
      }

      if (values.count() > 0) {
        // LOG_INFO("[FTS_GENERIC_FILTER] extracted IN condition",
        //          K(column_idx), "value_count", values.count());
        return add_condition(column_idx, FTS_OP_IN, values);
      }
      return common::OB_SUCCESS;
    }

    // ===== NOT IN 操作 =====
    case T_OP_NOT_IN: {
      // 类似 IN 处理
      if (expr->arg_cnt_ < 2 || expr->args_[0]->type_ != T_REF_COLUMN) {
        return common::OB_NOT_SUPPORTED;
      }
      column_expr = expr->args_[0];
      op_type = FTS_OP_NOT_IN;

      if (OB_FAIL(find_column_index(column_expr, output_exprs, cache, column_idx))) {
        return ret;
      }

      common::ObArray<ObFtsColumnValue> values;
      values.reserve(expr->arg_cnt_ - 1);  // 预分配空间
      for (int64_t k = 1; k < expr->arg_cnt_; ++k) {
        ObExpr *val_expr = expr->args_[k];
        if (OB_ISNULL(val_expr)) continue;
        if (val_expr->type_ == T_OP_ROW) {
          for (int64_t m = 0; m < val_expr->arg_cnt_; ++m) {
            extract_const_value(val_expr->args_[m], eval_ctx, values);
          }
        } else {
          extract_const_value(val_expr, eval_ctx, values);
        }
      }

      if (values.count() > 0) {
        return add_condition(column_idx, FTS_OP_NOT_IN, values);
      }
      return common::OB_SUCCESS;
    }

    // ===== IS NULL / IS NOT NULL（使用无值快速路径）=====
    case T_OP_IS: {
      if (expr->arg_cnt_ >= 1 && expr->args_[0]->type_ == T_REF_COLUMN) {
        column_expr = expr->args_[0];
        op_type = FTS_OP_IS_NULL;

        if (OB_FAIL(find_column_index(column_expr, output_exprs, cache, column_idx))) {
          return ret;
        }

        // 快速路径：无值条件不需要创建 ObArray
        return add_no_value_condition(column_idx, op_type);
      }
      return common::OB_NOT_SUPPORTED;
    }

    case T_OP_IS_NOT: {
      if (expr->arg_cnt_ >= 1 && expr->args_[0]->type_ == T_REF_COLUMN) {
        column_expr = expr->args_[0];
        op_type = FTS_OP_IS_NOT_NULL;

        if (OB_FAIL(find_column_index(column_expr, output_exprs, cache, column_idx))) {
          return ret;
        }

        // 快速路径：无值条件不需要创建 ObArray
        return add_no_value_condition(column_idx, op_type);
      }
      return common::OB_NOT_SUPPORTED;
    }

    default:
      return common::OB_NOT_SUPPORTED;
  }

  // 处理简单比较操作（使用快速路径：避免创建临时 ObArray）
  if (OB_NOT_NULL(column_expr) && OB_NOT_NULL(value_expr) && op_type != FTS_OP_NONE) {
    if (OB_FAIL(find_column_index(column_expr, output_exprs, cache, column_idx))) {
      LOG_DEBUG("[FTS_GENERIC_FILTER] cannot find column for comparison",
                K(ret), K(column_expr->type_));
      return ret;
    }

    // 快速路径：直接提取单个值，避免创建临时 ObArray
    ObDatum *datum = nullptr;
    if (OB_FAIL(value_expr->eval(eval_ctx, datum))) {
      LOG_DEBUG("[FTS_GENERIC_FILTER] failed to eval const expr", K(ret));
      return ret;
    }

    if (OB_ISNULL(datum) || datum->is_null()) {
      return common::OB_SUCCESS;  // NULL 值，跳过
    }

    ObFtsColumnValue val;
    if (OB_FAIL(ObFtsColumnValue::from_datum_shallow(*datum, value_expr->datum_meta_.type_, val))) {
      LOG_DEBUG("[FTS_GENERIC_FILTER] failed to convert datum", K(ret));
      return ret;
    }

    // 使用单值快速路径添加条件
    /*LOG_DEBUG("[FTS_GENERIC_FILTER] using single value fast path",
              K(column_idx), "op", get_filter_op_name(op_type));*/
    return add_single_value_condition(column_idx, op_type, val);
  }

  return common::OB_NOT_SUPPORTED;
}

int ObFtsGenericFilterCtx::find_column_index(
    ObExpr *expr,
    const common::ObIArray<ObExpr*> &output_exprs,
    const ObFtsGenericCache &cache,
    int64_t &column_idx)
{
  int ret = common::OB_SUCCESS;
  column_idx = -1;

  if (OB_ISNULL(expr)) {
    return common::OB_INVALID_ARGUMENT;
  }

  // 获取目标表达式的类型
  common::ObObjType target_type = expr->datum_meta_.type_;
  
  // ========== 策略1：尝试在 output_exprs 中查找 ==========
  int64_t type_order = 0;
  bool found_in_output = false;
  
  for (int64_t i = 0; i < output_exprs.count(); ++i) {
    ObExpr *cur_expr = output_exprs.at(i);
    if (OB_ISNULL(cur_expr)) continue;
    
    // 只统计普通列引用
    if (cur_expr->type_ != T_REF_COLUMN) continue;
    
    if (cur_expr == expr) {
      found_in_output = true;
      break;
    }
    
    if (cur_expr->datum_meta_.type_ == target_type) {
      type_order++;
    }
  }

  if (found_in_output) {
    int64_t signature = ObFtsColumnMeta::make_signature(target_type, type_order);
    column_idx = cache.find_column_by_signature(signature);
    if (column_idx >= 0) {
      // LOG_INFO("[FTS_GENERIC_FILTER] column matched via output_exprs",
      //          K(column_idx), K(target_type), K(type_order), K(signature));
      return common::OB_SUCCESS;
    }
  }

  // ========== 策略2：直接使用类型匹配（不依赖 output_exprs）==========
  // 当 WHERE 子句的列不在 SELECT 列表中时使用
  // 对于简单类型（整数），通常只有一个主键列，取 type_order=0
  // 对于字符串类型，依次尝试 type_order=0, 1, 2...
  
  // 统计缓存中同类型列的数量
  int64_t same_type_count = 0;
  for (int64_t i = 0; i < cache.column_count(); ++i) {
    if (cache.get_column_meta(i).obj_type_ == target_type) {
      same_type_count++;
    }
  }
  
  if (same_type_count == 0) {
    LOG_DEBUG("[FTS_GENERIC_FILTER] no column of this type in cache", K(target_type));
    return common::OB_ENTRY_NOT_EXIST;
  }
  
  // 对于整数类型（通常是主键 id），使用第一个同类型列
  // 对于字符串类型（可能有多个），使用第一个同类型列作为默认
  // 注意：如果有多个同类型列，可能需要更精确的匹配逻辑
  int64_t default_order = 0;
  
  // 特殊处理：对于 IN 条件，如果有多个 VARCHAR 列，可能需要选择正确的列
  // 这里我们使用启发式：
  // - 整数类型（BIGINT/INT）：取 type_order=0（通常是主键）
  // - 字符串类型（VARCHAR）：取 type_order=0（假设第一个 VARCHAR 是过滤列）
  
  int64_t signature = ObFtsColumnMeta::make_signature(target_type, default_order);
  column_idx = cache.find_column_by_signature(signature);
  
  if (column_idx >= 0) {
    // LOG_INFO("[FTS_GENERIC_FILTER] column matched by type fallback",
    //          K(column_idx), K(target_type), "default_order", default_order,
    //          K(signature), K(same_type_count));
    return common::OB_SUCCESS;
  }
  
  // 如果默认的 type_order=0 找不到，尝试其他序号
  for (int64_t order = 1; order < same_type_count; ++order) {
    signature = ObFtsColumnMeta::make_signature(target_type, order);
    column_idx = cache.find_column_by_signature(signature);
    if (column_idx >= 0) {
      // LOG_INFO("[FTS_GENERIC_FILTER] column matched by type fallback (order)",
      //          K(column_idx), K(target_type), K(order), K(signature));
      return common::OB_SUCCESS;
    }
  }

  LOG_DEBUG("[FTS_GENERIC_FILTER] column not found by any method", K(target_type));
  return common::OB_ENTRY_NOT_EXIST;
}

int ObFtsGenericFilterCtx::extract_const_value(
    ObExpr *expr,
    ObEvalCtx &eval_ctx,
    common::ObIArray<ObFtsColumnValue> &values)
{
  int ret = common::OB_SUCCESS;

  if (OB_ISNULL(expr)) {
    return common::OB_INVALID_ARGUMENT;
  }

  ObDatum *datum = nullptr;
  if (OB_FAIL(expr->eval(eval_ctx, datum))) {
    LOG_DEBUG("[FTS_GENERIC_FILTER] failed to eval const expr", K(ret));
    return ret;
  }

  if (OB_ISNULL(datum) || datum->is_null()) {
    // NULL 值，跳过
    return common::OB_SUCCESS;
  }

  ObFtsColumnValue val;
  // 使用浅拷贝，避免双重内存分配（深拷贝在 add_condition 中完成）
  if (OB_FAIL(ObFtsColumnValue::from_datum_shallow(*datum, expr->datum_meta_.type_, val))) {
    LOG_DEBUG("[FTS_GENERIC_FILTER] failed to convert datum", K(ret));
    return ret;
  }

  if (OB_FAIL(values.push_back(val))) {
    LOG_WARN("[FTS_GENERIC_FILTER] failed to push back value", K(ret));
  }

  return ret;
}

ObFtsFilterOpType ObFtsGenericFilterCtx::convert_expr_type(ObExprOperatorType type)
{
  switch (type) {
    case T_OP_LT: return FTS_OP_LT;
    case T_OP_LE: return FTS_OP_LE;
    case T_OP_GT: return FTS_OP_GT;
    case T_OP_GE: return FTS_OP_GE;
    case T_OP_EQ: return FTS_OP_EQ;
    case T_OP_NE: return FTS_OP_NE;
    case T_OP_NSEQ: return FTS_OP_EQ;  // NULL-safe equal
    case T_OP_IN: return FTS_OP_IN;
    case T_OP_NOT_IN: return FTS_OP_NOT_IN;
    case T_OP_IS: return FTS_OP_IS_NULL;
    case T_OP_IS_NOT: return FTS_OP_IS_NOT_NULL;
    default: return FTS_OP_NONE;
  }
}

ObFtsFilterOpType ObFtsGenericFilterCtx::flip_op(ObFtsFilterOpType op)
{
  switch (op) {
    case FTS_OP_LT: return FTS_OP_GT;
    case FTS_OP_LE: return FTS_OP_GE;
    case FTS_OP_GT: return FTS_OP_LT;
    case FTS_OP_GE: return FTS_OP_LE;
    default: return op;  // EQ, NE 等对称操作不变
  }
}

} // namespace sql
} // namespace oceanbase
