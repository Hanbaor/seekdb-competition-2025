/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用缓存实现
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_generic_cache.h"
#include "lib/allocator/ob_malloc.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{

// 全局单例
ObFtsGenericCache g_fts_generic_cache;

// 静态成员变量定义（链接器需要）
const int64_t ObFtsGenericCache::DEFAULT_CAPACITY;
const int64_t ObFtsGenericCache::MAX_COLUMNS;

// 静态空列元数据
static ObFtsColumnMeta EMPTY_COLUMN_META;

ObFtsGenericCache::ObFtsGenericCache()
  : is_inited_(false),
    mem_attr_(OB_SERVER_TENANT_ID, "FtsGenCache"),
    allocator_("FtsGenCacheAl"),
    column_count_(0),
    count_(0),
    capacity_(0),
    lock_(common::ObLatchIds::DEFAULT_SPIN_LOCK)
{
  for (int64_t i = 0; i < MAX_COLUMNS; ++i) {
    column_data_[i] = nullptr;
  }
}

ObFtsGenericCache::~ObFtsGenericCache()
{
  destroy();
}

int ObFtsGenericCache::init(const common::ObIArray<ObFtsColumnMeta> &column_metas,
                            int64_t capacity)
{
  int ret = common::OB_SUCCESS;

  if (is_inited_) {
    // 如果已经初始化，先检查列数是否一致
    if (column_count_ == column_metas.count()) {
      LOG_INFO("[FTS_GENERIC_CACHE] cache already inited with same column count, reusing",
               K_(column_count), K_(capacity), K_(count));
      return common::OB_SUCCESS;
    }
    LOG_INFO("[FTS_GENERIC_CACHE] cache already inited with different columns, resetting",
             "old_col_count", column_count_, "new_col_count", column_metas.count());
    reuse();
  }

  if (column_metas.count() <= 0) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] no columns to cache", K(ret));
    return ret;
  }

  if (column_metas.count() > MAX_COLUMNS) {
    ret = common::OB_SIZE_OVERFLOW;
    LOG_WARN("[FTS_GENERIC_CACHE] too many columns", K(ret),
             "count", column_metas.count(), K(MAX_COLUMNS));
    return ret;
  }

  common::ObSpinLockGuard guard(lock_);

  // 复制列元数据
  column_count_ = column_metas.count();
  for (int64_t i = 0; i < column_count_; ++i) {
    column_metas_[i] = column_metas.at(i);
  }

  // 分配列存储
  capacity_ = capacity > 0 ? capacity : DEFAULT_CAPACITY;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count_; ++i) {
    void *buf = ob_malloc(sizeof(ObFtsColumnValue) * capacity_, mem_attr_);
    if (OB_ISNULL(buf)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("[FTS_GENERIC_CACHE] failed to allocate column data", K(ret), K(i), K_(capacity));
    } else {
      column_data_[i] = static_cast<ObFtsColumnValue*>(buf);
      // 初始化为 NULL
      for (int64_t j = 0; j < static_cast<int64_t>(capacity_); ++j) {
        new (&column_data_[i][j]) ObFtsColumnValue();
      }
    }
  }

  if (OB_SUCC(ret)) {
    count_ = 0;
    is_inited_ = true;
    LOG_INFO("[FTS_GENERIC_CACHE] cache initialized successfully",
             K_(column_count), K_(capacity),
             "column_metas", common::ObArrayWrap<ObFtsColumnMeta>(column_metas_, column_count_));
  } else {
    // 清理已分配的内存
    for (int64_t i = 0; i < MAX_COLUMNS; ++i) {
      if (OB_NOT_NULL(column_data_[i])) {
        ob_free(column_data_[i]);
        column_data_[i] = nullptr;
      }
    }
  }

  return ret;
}

int ObFtsGenericCache::add_row(uint64_t doc_id, const ObFtsColumnValue *column_values)
{
  int ret = common::OB_SUCCESS;

  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }

  if (doc_id == 0 || OB_ISNULL(column_values)) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }

  common::ObSpinLockGuard guard(lock_);

  // 确保容量足够
  if (OB_FAIL(ensure_capacity(doc_id))) {
    LOG_WARN("[FTS_GENERIC_CACHE] failed to ensure capacity", K(ret), K(doc_id));
    return ret;
  }

  uint64_t row_idx = doc_id - 1;

  // 复制每列的值
  for (int64_t col_idx = 0; OB_SUCC(ret) && col_idx < column_count_; ++col_idx) {
    const ObFtsColumnValue &src = column_values[col_idx];

    if (src.type_ == ObFtsColumnValue::TYPE_STRING) {
      // 字符串需要深拷贝
      if (OB_FAIL(src.deep_copy(allocator_, column_data_[col_idx][row_idx]))) {
        LOG_WARN("[FTS_GENERIC_CACHE] failed to deep copy string", K(ret), K(col_idx));
      }
    } else {
      column_data_[col_idx][row_idx] = src;
    }
  }

  if (OB_SUCC(ret) && doc_id > count_) {
    count_ = doc_id;
  }

  return ret;
}

int ObFtsGenericCache::add_row_from_exprs(uint64_t doc_id,
                                          const common::ObIArray<ObExpr*> &output_exprs,
                                          ObEvalCtx &eval_ctx)
{
  int ret = common::OB_SUCCESS;

  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }

  if (doc_id == 0) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }

  common::ObSpinLockGuard guard(lock_);

  // 确保容量足够
  if (OB_FAIL(ensure_capacity(doc_id))) {
    LOG_WARN("[FTS_GENERIC_CACHE] failed to ensure capacity", K(ret), K(doc_id));
    return ret;
  }

  uint64_t row_idx = doc_id - 1;

  // 遍历每个缓存列，从对应的表达式获取值
  for (int64_t col_idx = 0; col_idx < column_count_; ++col_idx) {
    const ObFtsColumnMeta &meta = column_metas_[col_idx];

    if (meta.output_idx_ < 0 ||
        meta.output_idx_ >= output_exprs.count()) {
      // 索引无效，设为 NULL
      column_data_[col_idx][row_idx] = ObFtsColumnValue::make_null();
      continue;
    }

    ObExpr *expr = output_exprs.at(meta.output_idx_);
    if (OB_ISNULL(expr)) {
      column_data_[col_idx][row_idx] = ObFtsColumnValue::make_null();
      continue;
    }

    ObDatum *datum = nullptr;
    int eval_ret = expr->eval(eval_ctx, datum);
    if (OB_SUCCESS != eval_ret) {
      LOG_DEBUG("[FTS_GENERIC_CACHE] failed to eval expr, using NULL",
                K(eval_ret), K(col_idx), K(doc_id));
      column_data_[col_idx][row_idx] = ObFtsColumnValue::make_null();
      continue;
    }

    if (OB_ISNULL(datum) || datum->is_null()) {
      column_data_[col_idx][row_idx] = ObFtsColumnValue::make_null();
    } else {
      ObFtsColumnValue val;
      int conv_ret = ObFtsColumnValue::from_datum(*datum, meta.obj_type_, allocator_, val);
      if (OB_SUCCESS != conv_ret) {
        LOG_DEBUG("[FTS_GENERIC_CACHE] failed to convert datum, using NULL",
                  K(conv_ret), K(col_idx), K(doc_id));
        column_data_[col_idx][row_idx] = ObFtsColumnValue::make_null();
      } else {
        column_data_[col_idx][row_idx] = val;
      }
    }
  }

  if (doc_id > count_) {
    count_ = doc_id;
  }

  // 采样输出日志
  if (count_ == 1 || count_ % 50000 == 0) {
    LOG_INFO("[FTS_GENERIC_CACHE] cache filling progress",
             K_(count), K_(capacity), K_(column_count));
  }

  return ret;
}

int ObFtsGenericCache::get_column_value(uint64_t doc_id, int64_t column_idx,
                                        ObFtsColumnValue &out) const
{
  int ret = common::OB_SUCCESS;

  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }

  if (doc_id == 0 || doc_id > count_) {
    ret = common::OB_ENTRY_NOT_EXIST;
    return ret;
  }

  if (column_idx < 0 || column_idx >= column_count_) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }

  if (OB_ISNULL(column_data_[column_idx])) {
    ret = common::OB_ERR_UNEXPECTED;
    return ret;
  }

  out = column_data_[column_idx][doc_id - 1];
  return ret;
}

int ObFtsGenericCache::get_row(uint64_t doc_id, ObFtsRowData &out) const
{
  int ret = common::OB_SUCCESS;

  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }

  if (doc_id == 0 || doc_id > count_) {
    ret = common::OB_ENTRY_NOT_EXIST;
    return ret;
  }

  // 注意：这里返回的是指向内部数据的指针，调用者不应修改
  // 实际上 ObFtsRowData 应该分配内存来存储数据，但为了性能，这里做浅拷贝
  static thread_local ObFtsColumnValue row_buffer[MAX_COLUMNS];

  uint64_t row_idx = doc_id - 1;
  for (int64_t col_idx = 0; col_idx < column_count_; ++col_idx) {
    if (OB_NOT_NULL(column_data_[col_idx])) {
      row_buffer[col_idx] = column_data_[col_idx][row_idx];
    } else {
      row_buffer[col_idx] = ObFtsColumnValue::make_null();
    }
  }

  out.columns_ = row_buffer;
  out.column_count_ = column_count_;

  return ret;
}

int ObFtsGenericCache::batch_get_column(const common::ObIArray<uint64_t> &doc_ids,
                                        int64_t column_idx,
                                        common::ObIArray<ObFtsColumnValue> &out) const
{
  int ret = common::OB_SUCCESS;

  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }

  if (column_idx < 0 || column_idx >= column_count_) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }

  out.reuse();

  for (int64_t i = 0; OB_SUCC(ret) && i < doc_ids.count(); ++i) {
    uint64_t doc_id = doc_ids.at(i);
    ObFtsColumnValue val;

    if (doc_id > 0 && doc_id <= count_ && OB_NOT_NULL(column_data_[column_idx])) {
      val = column_data_[column_idx][doc_id - 1];
    } else {
      val = ObFtsColumnValue::make_null();
    }

    if (OB_FAIL(out.push_back(val))) {
      LOG_WARN("[FTS_GENERIC_CACHE] failed to push back value", K(ret), K(i));
    }
  }

  return ret;
}

const ObFtsColumnMeta& ObFtsGenericCache::get_column_meta(int64_t idx) const
{
  if (idx >= 0 && idx < column_count_) {
    return column_metas_[idx];
  }
  return EMPTY_COLUMN_META;
}

int ObFtsGenericCache::ensure_capacity(uint64_t required)
{
  int ret = common::OB_SUCCESS;

  if (required <= capacity_) {
    return ret;
  }

  // 计算新容量（至少翻倍）
  uint64_t new_capacity = capacity_ * 2;
  while (new_capacity < required) {
    new_capacity *= 2;
  }

  LOG_INFO("[FTS_GENERIC_CACHE] expanding capacity",
           K_(capacity), K(new_capacity), K(required));

  // 重新分配每列的数组
  for (int64_t col_idx = 0; OB_SUCC(ret) && col_idx < column_count_; ++col_idx) {
    void *new_buf = ob_malloc(sizeof(ObFtsColumnValue) * new_capacity, mem_attr_);
    if (OB_ISNULL(new_buf)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("[FTS_GENERIC_CACHE] failed to allocate new buffer", K(ret), K(col_idx));
    } else {
      ObFtsColumnValue *new_data = static_cast<ObFtsColumnValue*>(new_buf);

      // 初始化新数组
      for (uint64_t j = 0; j < new_capacity; ++j) {
        new (&new_data[j]) ObFtsColumnValue();
      }

      // 复制旧数据
      if (OB_NOT_NULL(column_data_[col_idx]) && capacity_ > 0) {
        for (uint64_t j = 0; j < capacity_; ++j) {
          new_data[j] = column_data_[col_idx][j];
        }
        ob_free(column_data_[col_idx]);
      }

      column_data_[col_idx] = new_data;
    }
  }

  if (OB_SUCC(ret)) {
    capacity_ = new_capacity;
  }

  return ret;
}

void ObFtsGenericCache::reuse()
{
  common::ObSpinLockGuard guard(lock_);

  // 重置所有列的值为 NULL
  for (int64_t col_idx = 0; col_idx < column_count_; ++col_idx) {
    if (OB_NOT_NULL(column_data_[col_idx])) {
      for (uint64_t j = 0; j < capacity_; ++j) {
        column_data_[col_idx][j] = ObFtsColumnValue::make_null();
      }
    }
  }

  count_ = 0;
  allocator_.reset();

  LOG_INFO("[FTS_GENERIC_CACHE] cache reused", K_(column_count), K_(capacity));
}

void ObFtsGenericCache::destroy()
{
  common::ObSpinLockGuard guard(lock_);

  for (int64_t i = 0; i < MAX_COLUMNS; ++i) {
    if (OB_NOT_NULL(column_data_[i])) {
      ob_free(column_data_[i]);
      column_data_[i] = nullptr;
    }
  }

  column_count_ = 0;
  count_ = 0;
  capacity_ = 0;
  is_inited_ = false;
  allocator_.reset();

  LOG_INFO("[FTS_GENERIC_CACHE] cache destroyed");
}

} // namespace sql
} // namespace oceanbase
