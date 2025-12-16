/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用缓存实现
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_generic_cache.h"
#include "lib/allocator/ob_malloc.h"
#include "share/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include <fcntl.h>
#include <unistd.h>

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
    lock_(common::ObLatchIds::DEFAULT_SPIN_LOCK),
    persist_file_opened_(false),
    row_index_(nullptr),
    cache_allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "FtsGenKVCache"))
{
  for (int64_t i = 0; i < MAX_COLUMNS; ++i) {
    column_data_[i] = nullptr;
  }
  memset(cache_file_path_, 0, sizeof(cache_file_path_));
  file_header_.reset();
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

  // 关闭持久化文件
  close_cache_file();

  column_count_ = 0;
  count_ = 0;
  capacity_ = 0;
  is_inited_ = false;
  allocator_.reset();
  cache_allocator_.reset();

  LOG_INFO("[FTS_GENERIC_CACHE] cache destroyed");
}

// ========================================================================
// V2: 持久化实现
// ========================================================================

int ObFtsGenericCache::get_cache_path(char *path_buf, int64_t buf_len)
{
  // 使用与 BM25S 索引相同的目录
  const char *data_dir = "/data/obcluster/store";
  
  int n = snprintf(path_buf, buf_len, "%s/fts_scalar_cache", data_dir);
  if (n < 0 || n >= buf_len) {
    return common::OB_SIZE_OVERFLOW;
  }
  
  return common::OB_SUCCESS;
}

int ObFtsGenericCache::persist_to_disk(const char *cache_dir)
{
  int ret = common::OB_SUCCESS;
  
  if (OB_ISNULL(cache_dir)) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: cache_dir is null", K(ret));
    return ret;
  }
  
  if (!is_inited_ || count_ == 0) {
    ret = common::OB_NOT_INIT;
    LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: not initialized or empty", 
             K(ret), K_(is_inited), K_(count));
    return ret;
  }
  
  LOG_INFO("[FTS_GENERIC_CACHE] persist_to_disk: starting", 
           K(cache_dir), K_(count), K_(column_count));
  
  int64_t start_time = common::ObTimeUtility::current_time();
  
  // 1. 创建目录
  if (OB_FAIL(share::ObIODeviceLocalFileOp::mkdir(cache_dir, 0755))) {
    if (OB_FILE_OR_DIRECTORY_EXIST == ret) {
      ret = common::OB_SUCCESS;
      LOG_INFO("[FTS_GENERIC_CACHE] persist_to_disk: directory exists", K(cache_dir));
    } else {
      LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to mkdir", K(ret), K(cache_dir));
      return ret;
    }
  }
  
  // 2. 构建文件路径
  char file_path[512];
  int n = snprintf(file_path, sizeof(file_path), "%s/fts_scalar_cache.dat", cache_dir);
  if (n < 0 || n >= static_cast<int>(sizeof(file_path))) {
    ret = common::OB_SIZE_OVERFLOW;
    LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: path too long", K(ret));
    return ret;
  }
  
  LOG_INFO("[FTS_GENERIC_CACHE] persist_to_disk: opening file", K(file_path));
  
  // 3. 打开文件
  common::ObIOFd fd;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(file_path, 
                                          O_CREAT | O_WRONLY | O_TRUNC, 
                                          0644, fd))) {
    LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to open file", K(ret), K(file_path));
    return ret;
  }
  
  // 4. 写入各个 Section
  do {
    if (OB_FAIL(write_header(fd))) {
      LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to write header", K(ret));
      break;
    }
    
    if (OB_FAIL(write_column_meta(fd))) {
      LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to write column meta", K(ret));
      break;
    }
    
    if (OB_FAIL(write_row_index_and_data(fd))) {
      LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to write row data", K(ret));
      break;
    }
    
    if (OB_FAIL(update_header(fd))) {
      LOG_WARN("[FTS_GENERIC_CACHE] persist_to_disk: fail to update header", K(ret));
      break;
    }
  } while (0);
  
  // 5. 同步并关闭
  if (fd.is_normal_file()) {
    share::ObIODeviceLocalFileOp::fsync(fd);
    share::ObIODeviceLocalFileOp::close(fd);
  }
  
  if (OB_SUCC(ret)) {
    MEMCPY(cache_file_path_, file_path, strlen(file_path) + 1);
    
    int64_t end_time = common::ObTimeUtility::current_time();
    double elapsed_ms = (end_time - start_time) / 1000.0;
    
    LOG_INFO("[FTS_GENERIC_CACHE] persist_to_disk: completed successfully", 
             K(file_path), K_(count), K_(column_count),
             "row_data_size", file_header_.row_data_size,
             "elapsed_ms", elapsed_ms);
  }
  
  return ret;
}

int ObFtsGenericCache::write_header(const common::ObIOFd &fd)
{
  int ret = common::OB_SUCCESS;
  
  // 初始化文件头
  file_header_.reset();
  file_header_.magic = FtsScalarCacheFileHeader::MAGIC;
  file_header_.version = FtsScalarCacheFileHeader::CURRENT_VERSION;
  file_header_.row_count = count_;
  file_header_.column_count = column_count_;
  file_header_.max_doc_id = count_;
  file_header_.create_timestamp = common::ObTimeUtility::current_time();
  
  // 计算各 section 的偏移
  file_header_.column_meta_offset = FtsScalarCacheFileHeader::HEADER_SIZE;
  file_header_.column_meta_size = column_count_ * sizeof(FtsColumnMetaEntry);
  file_header_.row_index_offset = file_header_.column_meta_offset + file_header_.column_meta_size;
  file_header_.row_index_size = count_ * sizeof(FtsRowIndexEntry);
  file_header_.row_data_offset = file_header_.row_index_offset + file_header_.row_index_size;
  // row_data_size 在 write_row_index_and_data 中更新
  
  // 写入 header
  int64_t write_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          fd.second_id_, &file_header_, sizeof(file_header_), 0, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] write_header: fail to write", K(ret));
  } else {
    LOG_INFO("[FTS_GENERIC_CACHE] write_header: completed", K(file_header_));
  }
  
  return ret;
}

int ObFtsGenericCache::write_column_meta(const common::ObIOFd &fd)
{
  int ret = common::OB_SUCCESS;
  
  FtsColumnMetaEntry *entries = static_cast<FtsColumnMetaEntry*>(
      allocator_.alloc(sizeof(FtsColumnMetaEntry) * column_count_));
  if (OB_ISNULL(entries)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] write_column_meta: fail to alloc", K(ret));
    return ret;
  }
  
  for (int64_t i = 0; i < column_count_; ++i) {
    entries[i].column_type = static_cast<int32_t>(column_metas_[i].obj_type_);
    entries[i].type_order = static_cast<int32_t>(column_metas_[i].type_order_);
    entries[i].type_signature = column_metas_[i].type_signature_;
    entries[i].is_primary_key = column_metas_[i].is_primary_key_ ? 1 : 0;
    entries[i].is_nullable = column_metas_[i].is_nullable_ ? 1 : 0;
  }
  
  int64_t write_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          fd.second_id_, entries, sizeof(FtsColumnMetaEntry) * column_count_,
          file_header_.column_meta_offset, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] write_column_meta: fail to write", K(ret));
  } else {
    LOG_INFO("[FTS_GENERIC_CACHE] write_column_meta: completed", K_(column_count));
  }
  
  allocator_.free(entries);
  return ret;
}

int ObFtsGenericCache::write_row_index_and_data(const common::ObIOFd &fd)
{
  int ret = common::OB_SUCCESS;
  
  // 1. 分配 Row Index 数组
  int64_t index_size = count_ * sizeof(FtsRowIndexEntry);
  FtsRowIndexEntry *row_index = static_cast<FtsRowIndexEntry*>(
      allocator_.alloc(index_size));
  if (OB_ISNULL(row_index)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] write_row_index_and_data: fail to alloc row_index", K(ret));
    return ret;
  }
  
  // 2. 分配行数据缓冲区
  const int64_t ROW_BUF_SIZE = 4096;  // 假设单行不超过 4KB
  char *row_buf = static_cast<char*>(allocator_.alloc(ROW_BUF_SIZE));
  if (OB_ISNULL(row_buf)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] write_row_index_and_data: fail to alloc row_buf", K(ret));
    allocator_.free(row_index);
    return ret;
  }
  
  // 3. 遍历每行，序列化并写入
  int64_t data_offset = 0;
  int64_t write_offset = file_header_.row_data_offset;
  
  for (uint64_t doc_id = 1; OB_SUCC(ret) && doc_id <= count_; ++doc_id) {
    int64_t row_size = 0;
    
    if (OB_FAIL(serialize_row(doc_id, row_buf, ROW_BUF_SIZE, row_size))) {
      LOG_WARN("[FTS_GENERIC_CACHE] write_row_index_and_data: fail to serialize row", 
               K(ret), K(doc_id));
      break;
    }
    
    // 记录索引
    row_index[doc_id - 1].data_offset = data_offset;
    row_index[doc_id - 1].data_size = row_size;
    
    // 写入行数据
    int64_t write_size = 0;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
            fd.second_id_, row_buf, row_size, write_offset, write_size))) {
      LOG_WARN("[FTS_GENERIC_CACHE] write_row_index_and_data: fail to write row", 
               K(ret), K(doc_id));
      break;
    }
    
    data_offset += row_size;
    write_offset += row_size;
    
    // 每处理 50000 行打印一次进度
    if (doc_id % 50000 == 0) {
      LOG_INFO("[FTS_GENERIC_CACHE] write_row_index_and_data: progress", 
               K(doc_id), K_(count), K(data_offset));
    }
  }
  
  // 4. 写入 Row Index
  if (OB_SUCC(ret)) {
    int64_t write_size = 0;
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
            fd.second_id_, row_index, index_size,
            file_header_.row_index_offset, write_size))) {
      LOG_WARN("[FTS_GENERIC_CACHE] write_row_index_and_data: fail to write row_index", K(ret));
    }
  }
  
  // 5. 更新 header 中的 row_data_size
  file_header_.row_data_size = data_offset;
  
  allocator_.free(row_index);
  allocator_.free(row_buf);
  
  LOG_INFO("[FTS_GENERIC_CACHE] write_row_index_and_data: completed", 
           K_(count), K(data_offset));
  
  return ret;
}

int ObFtsGenericCache::serialize_row(uint64_t doc_id, char *buf, int64_t buf_len, int64_t &row_size)
{
  int ret = common::OB_SUCCESS;
  char *p = buf;
  char *end = buf + buf_len;
  
  uint64_t row_idx = doc_id - 1;
  
  for (int64_t col_idx = 0; col_idx < column_count_ && p < end; ++col_idx) {
    if (OB_ISNULL(column_data_[col_idx])) {
      // 写入 NULL
      *p++ = static_cast<char>(ObFtsColumnValue::TYPE_NULL);
      continue;
    }
    
    const ObFtsColumnValue &val = column_data_[col_idx][row_idx];
    
    // 写入类型标记
    *p++ = static_cast<char>(val.type_);
    
    // 写入值
    switch (val.type_) {
      case ObFtsColumnValue::TYPE_NULL:
        // 无额外数据
        break;
      case ObFtsColumnValue::TYPE_INT64:
        if (p + sizeof(int64_t) <= end) {
          memcpy(p, &val.int_val_, sizeof(int64_t));
          p += sizeof(int64_t);
        }
        break;
      case ObFtsColumnValue::TYPE_UINT64:
        if (p + sizeof(uint64_t) <= end) {
          memcpy(p, &val.uint_val_, sizeof(uint64_t));
          p += sizeof(uint64_t);
        }
        break;
      case ObFtsColumnValue::TYPE_DOUBLE:
        if (p + sizeof(double) <= end) {
          memcpy(p, &val.double_val_, sizeof(double));
          p += sizeof(double);
        }
        break;
      case ObFtsColumnValue::TYPE_STRING: {
        int32_t len = val.str_val_.len_;
        if (p + sizeof(int32_t) + len <= end) {
          memcpy(p, &len, sizeof(int32_t));
          p += sizeof(int32_t);
          if (len > 0 && OB_NOT_NULL(val.str_val_.ptr_)) {
            memcpy(p, val.str_val_.ptr_, len);
            p += len;
          }
        }
        break;
      }
      case ObFtsColumnValue::TYPE_DATETIME:
        if (p + sizeof(int64_t) <= end) {
          memcpy(p, &val.datetime_val_, sizeof(int64_t));
          p += sizeof(int64_t);
        }
        break;
      default:
        break;
    }
  }
  
  row_size = p - buf;
  return ret;
}

int ObFtsGenericCache::update_header(const common::ObIOFd &fd)
{
  int ret = common::OB_SUCCESS;
  int64_t write_size = 0;
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          fd.second_id_, &file_header_, sizeof(file_header_), 0, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] update_header: fail to write", K(ret));
  } else {
    LOG_INFO("[FTS_GENERIC_CACHE] update_header: completed", K(file_header_));
  }
  
  return ret;
}

int ObFtsGenericCache::load_from_disk(const char *cache_dir)
{
  int ret = common::OB_SUCCESS;
  
  // 如果已经加载过，直接返回
  if (persist_file_opened_) {
    LOG_INFO("[FTS_GENERIC_CACHE] load_from_disk: already loaded");
    return common::OB_SUCCESS;
  }
  
  if (OB_ISNULL(cache_dir)) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: cache_dir is null", K(ret));
    return ret;
  }
  
  // 1. 构建文件路径
  char file_path[512];
  snprintf(file_path, sizeof(file_path), "%s/fts_scalar_cache.dat", cache_dir);
  
  LOG_INFO("[FTS_GENERIC_CACHE] load_from_disk: starting", K(file_path));
  
  // 2. 检查文件是否存在
  bool is_exist = false;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::exist(file_path, is_exist))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: fail to check file exist", K(ret), K(file_path));
    return ret;
  }
  if (!is_exist) {
    ret = common::OB_FILE_NOT_EXIST;
    LOG_INFO("[FTS_GENERIC_CACHE] load_from_disk: cache file not exist", K(ret), K(file_path));
    return ret;
  }
  
  // 3. 打开文件
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(file_path, O_RDONLY, 0, persist_fd_))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: fail to open file", K(ret), K(file_path));
    return ret;
  }
  
  // 4. 读取 Header
  int64_t read_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(
          persist_fd_.second_id_, &file_header_, sizeof(file_header_), 0, read_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: fail to read header", K(ret));
    close_cache_file();
    return ret;
  }
  
  // 5. 验证 Header
  if (!file_header_.is_valid()) {
    ret = common::OB_INVALID_DATA;
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: invalid header", K(ret),
             K(file_header_.magic), K(file_header_.version));
    close_cache_file();
    return ret;
  }
  
  // 6. 加载 Row Index
  int64_t index_size = file_header_.row_count * sizeof(FtsRowIndexEntry);
  row_index_ = static_cast<FtsRowIndexEntry*>(cache_allocator_.alloc(index_size));
  if (OB_ISNULL(row_index_)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: fail to alloc row_index", K(ret), K(index_size));
    close_cache_file();
    return ret;
  }
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(
          persist_fd_.second_id_, row_index_, index_size,
          file_header_.row_index_offset, read_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_from_disk: fail to read row_index", K(ret));
    close_cache_file();
    return ret;
  }
  
  // 7. 加载列元数据（可选，用于验证）
  FtsColumnMetaEntry *col_entries = static_cast<FtsColumnMetaEntry*>(
      cache_allocator_.alloc(sizeof(FtsColumnMetaEntry) * file_header_.column_count));
  if (OB_NOT_NULL(col_entries)) {
    if (OB_SUCC(share::ObIODeviceLocalFileOp::pread_impl(
            persist_fd_.second_id_, col_entries, 
            sizeof(FtsColumnMetaEntry) * file_header_.column_count,
            file_header_.column_meta_offset, read_size))) {
      // 恢复列元数据
      column_count_ = file_header_.column_count;
      for (int64_t i = 0; i < column_count_; ++i) {
        column_metas_[i].obj_type_ = static_cast<common::ObObjType>(col_entries[i].column_type);
        column_metas_[i].type_order_ = col_entries[i].type_order;
        column_metas_[i].type_signature_ = col_entries[i].type_signature;
        column_metas_[i].is_primary_key_ = (col_entries[i].is_primary_key != 0);
        column_metas_[i].is_nullable_ = (col_entries[i].is_nullable != 0);
      }
    }
  }
  
  // 8. 更新状态
  // 注意：不要设置 is_inited_ = true！
  // is_inited_ 表示内存中的 column_data_[] 数组已分配并填充数据
  // 从磁盘加载时，内存中没有数据，只有元数据，应该使用 KVCache 模式获取数据
  // persist_file_opened_ = true 表示可以使用 KVCache 模式
  if (OB_SUCC(ret)) {
    // count_ 仍然设置为 0，因为内存中没有数据
    // 使用 file_header_.row_count 表示文件中的行数
    MEMCPY(cache_file_path_, file_path, strlen(file_path) + 1);
    persist_file_opened_ = true;
    // is_inited_ 保持 false，这样检测逻辑会使用 KVCache Mode
    
    LOG_INFO("[FTS_GENERIC_CACHE] load_from_disk: completed successfully (KVCache Mode enabled)",
             K(file_path), K(file_header_.row_count), K(file_header_.column_count),
             "is_inited", is_inited_, "persist_file_opened", persist_file_opened_);
  }
  
  return ret;
}

void ObFtsGenericCache::close_cache_file()
{
  if (persist_file_opened_ && persist_fd_.is_normal_file()) {
    share::ObIODeviceLocalFileOp::close(persist_fd_);
    persist_file_opened_ = false;
  }
  if (OB_NOT_NULL(row_index_)) {
    cache_allocator_.free(row_index_);
    row_index_ = nullptr;
  }
}

// ========================================================================
// V2: KVCache 查询实现
// ========================================================================

int ObFtsGenericCache::get_row_from_kvcache(uint64_t doc_id,
                                            const ObFtsScalarCacheValue *&row,
                                            common::ObKVCacheHandle &handle)
{
  int ret = common::OB_SUCCESS;
  
  if (!persist_file_opened_) {
    // 尝试从磁盘加载
    ret = ensure_ready();
    if (OB_FAIL(ret)) {
      LOG_WARN("[FTS_GENERIC_CACHE] get_row_from_kvcache: fail to ensure ready", K(ret));
      return ret;
    }
  }
  
  if (!persist_file_opened_) {
    ret = common::OB_NOT_INIT;
    LOG_WARN("[FTS_GENERIC_CACHE] get_row_from_kvcache: cache file not opened", K(ret));
    return ret;
  }
  
  // 确保 KVCache 已初始化
  if (!is_fts_scalar_kvcache_inited()) {
    ret = init_fts_scalar_kvcache();
    if (OB_FAIL(ret)) {
      LOG_WARN("[FTS_GENERIC_CACHE] get_row_from_kvcache: fail to init kvcache", K(ret));
      return ret;
    }
  }
  
  ObFtsScalarKVCache *kvcache = get_fts_scalar_kvcache();
  if (OB_ISNULL(kvcache)) {
    ret = common::OB_ERR_UNEXPECTED;
    LOG_WARN("[FTS_GENERIC_CACHE] get_row_from_kvcache: kvcache is null", K(ret));
    return ret;
  }
  
  // 1. 尝试从缓存获取
  ObFtsScalarCacheKey key(doc_id);
  ret = kvcache->get(key, row, handle);
  
  if (common::OB_SUCCESS == ret) {
    // 缓存命中
    LOG_DEBUG("[FTS_GENERIC_CACHE] get_row_from_kvcache: cache HIT", K(doc_id));
    return common::OB_SUCCESS;
  }
  
  if (common::OB_ENTRY_NOT_EXIST == ret) {
    // 2. 缓存未命中，从文件加载
    LOG_DEBUG("[FTS_GENERIC_CACHE] get_row_from_kvcache: cache MISS, loading from file", K(doc_id));
    ret = load_row_to_kvcache(doc_id, row, handle);
  }
  
  return ret;
}

int ObFtsGenericCache::load_row_to_kvcache(uint64_t doc_id,
                                           const ObFtsScalarCacheValue *&row,
                                           common::ObKVCacheHandle &handle)
{
  int ret = common::OB_SUCCESS;
  
  if (doc_id == 0 || doc_id > static_cast<uint64_t>(file_header_.row_count)) {
    ret = common::OB_ENTRY_NOT_EXIST;
    return ret;
  }
  
  if (OB_ISNULL(row_index_)) {
    ret = common::OB_NOT_INIT;
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_kvcache: row_index is null", K(ret));
    return ret;
  }
  
  // 1. 从 Row Index 获取偏移和大小
  const FtsRowIndexEntry &index_entry = row_index_[doc_id - 1];
  
  if (index_entry.data_size <= 0) {
    ret = common::OB_ENTRY_NOT_EXIST;
    return ret;
  }
  
  // 2. 读取行数据
  char *row_data = static_cast<char*>(cache_allocator_.alloc(index_entry.data_size));
  if (OB_ISNULL(row_data)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_kvcache: fail to alloc row_data", K(ret));
    return ret;
  }
  
  int64_t data_offset = file_header_.row_data_offset + index_entry.data_offset;
  int64_t read_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(
          persist_fd_.second_id_, row_data, index_entry.data_size,
          data_offset, read_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_kvcache: fail to read row", K(ret), K(doc_id));
    cache_allocator_.free(row_data);
    return ret;
  }
  
  // 3. 构建临时 Value
  ObFtsScalarCacheValue tmp_value;
  tmp_value.set(file_header_.column_count, index_entry.data_size, row_data);
  
  // 4. 放入缓存
  ObFtsScalarCacheKey key(doc_id);
  ObFtsScalarKVCache *kvcache = get_fts_scalar_kvcache();
  
  ret = kvcache->put_and_fetch(key, tmp_value, row, handle);
  if (OB_FAIL(ret)) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_kvcache: fail to put to cache", K(ret), K(doc_id));
  } else {
    LOG_DEBUG("[FTS_GENERIC_CACHE] load_row_to_kvcache: row cached", K(doc_id));
  }
  
  // 5. 释放临时缓冲区（数据已被 deep_copy 到 KVCache）
  cache_allocator_.free(row_data);
  
  return ret;
}

int ObFtsGenericCache::prefetch_rows(const common::ObIArray<uint64_t> &doc_ids)
{
  int ret = common::OB_SUCCESS;
  
  if (!persist_file_opened_) {
    ret = ensure_ready();
    if (OB_FAIL(ret)) {
      return ret;
    }
  }
  
  ObFtsScalarKVCache *kvcache = get_fts_scalar_kvcache();
  if (OB_ISNULL(kvcache)) {
    return common::OB_NOT_INIT;
  }
  
  int64_t loaded_count = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < doc_ids.count(); ++i) {
    uint64_t doc_id = doc_ids.at(i);
    
    // 检查是否已在缓存中
    ObFtsScalarCacheKey key(doc_id);
    const ObFtsScalarCacheValue *row = nullptr;
    common::ObKVCacheHandle handle;
    
    if (common::OB_SUCCESS == kvcache->get(key, row, handle)) {
      // 已在缓存中，跳过
      continue;
    }
    
    // 加载到缓存
    if (OB_FAIL(load_row_to_kvcache(doc_id, row, handle))) {
      if (common::OB_ENTRY_NOT_EXIST == ret) {
        ret = common::OB_SUCCESS;  // 不存在的行不是错误
      }
    } else {
      loaded_count++;
    }
  }
  
  if (loaded_count > 0) {
    LOG_INFO("[FTS_GENERIC_CACHE] prefetch_rows: completed", 
             K(loaded_count), "total", doc_ids.count());
  }
  
  return ret;
}

int ObFtsGenericCache::ensure_ready()
{
  int ret = common::OB_SUCCESS;
  
  // 检查是否已经就绪
  if (persist_file_opened_) {
    return common::OB_SUCCESS;
  }
  
  // 如果内存缓存可用，也可以使用
  if (is_inited_ && count_ > 0) {
    return common::OB_SUCCESS;
  }
  
  // 尝试从默认路径加载
  char cache_dir[512];
  if (OB_FAIL(get_cache_path(cache_dir, sizeof(cache_dir)))) {
    LOG_WARN("[FTS_GENERIC_CACHE] ensure_ready: fail to get cache path", K(ret));
    return ret;
  }
  
  LOG_INFO("[FTS_GENERIC_CACHE] ensure_ready: attempting to load from disk", K(cache_dir));
  
  ret = load_from_disk(cache_dir);
  
  if (OB_SUCC(ret)) {
    LOG_INFO("[FTS_GENERIC_CACHE] ensure_ready: loaded from disk successfully",
             K_(cache_file_path), "row_count", file_header_.row_count);
    
    // 初始化 KVCache
    if (!is_fts_scalar_kvcache_inited()) {
      int cache_ret = init_fts_scalar_kvcache();
      if (OB_SUCCESS != cache_ret) {
        LOG_WARN("[FTS_GENERIC_CACHE] ensure_ready: fail to init kvcache (non-fatal)", K(cache_ret));
      }
    }
  } else if (common::OB_FILE_NOT_EXIST == ret) {
    LOG_INFO("[FTS_GENERIC_CACHE] ensure_ready: cache file not exist, will use memory mode");
    ret = common::OB_SUCCESS;  // 文件不存在不是错误
  } else {
    LOG_WARN("[FTS_GENERIC_CACHE] ensure_ready: fail to load from disk", K(ret));
  }
  
  return ret;
}

} // namespace sql
} // namespace oceanbase
