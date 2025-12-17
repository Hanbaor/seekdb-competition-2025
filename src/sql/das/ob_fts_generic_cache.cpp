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
    cache_allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "FtsGenKVCache")),
    streaming_mode_(false),
    streaming_data_offset_(0),
    streaming_row_data_start_(0),
    streaming_row_index_(nullptr),
    streaming_row_index_capacity_(0),
    streaming_row_buf_(nullptr),
    active_collectors_(0),
    hashmap_allocator_(ObMemAttr(OB_SERVER_TENANT_ID, "FtsHashMap")),
    hashmap_inited_(false),
    use_hashmap_(true),  // 默认启用 HashMap 模式
    hashmap_lock_(common::ObLatchIds::DEFAULT_SPIN_LOCK)
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
  
  // 关闭流式写入文件
  if (streaming_mode_ && streaming_fd_.is_normal_file()) {
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    streaming_mode_ = false;
  }
  
  // 释放流式存储缓冲区
  if (OB_NOT_NULL(streaming_row_index_)) {
    ob_free(streaming_row_index_);
    streaming_row_index_ = nullptr;
    streaming_row_index_capacity_ = 0;
  }
  if (OB_NOT_NULL(streaming_row_buf_)) {
    ob_free(streaming_row_buf_);
    streaming_row_buf_ = nullptr;
  }
  streaming_data_offset_ = 0;
  streaming_row_data_start_ = 0;

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
    // 缓存命中（热路径，移除日志）
    return common::OB_SUCCESS;
  }
  
  if (common::OB_ENTRY_NOT_EXIST == ret) {
    // 2. 缓存未命中，从文件加载（热路径，移除日志）
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
    // 热路径，移除日志
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

// ========================================================================
// V3: 流式存储实现
// ========================================================================

int ObFtsGenericCache::init_streaming_persist(const char *cache_dir,
                                              const common::ObIArray<ObFtsColumnMeta> &column_metas,
                                              int64_t estimated_rows)
{
  int ret = common::OB_SUCCESS;
  
  if (streaming_mode_) {
    ret = common::OB_INIT_TWICE;
    LOG_WARN("[FTS_GENERIC_CACHE] streaming persist already initialized", K(ret));
    return ret;
  }
  
  // 如果之前已经完成过（READY 状态），关闭旧文件准备重建
  if (persist_file_opened_) {
    close_cache_file();
    LOG_INFO("[FTS_GENERIC_CACHE] init_streaming_persist: closed existing cache file for rebuild");
  }
  
  // 重置并行任务计数器
  ATOMIC_SET(&active_collectors_, 0);
  
  if (OB_ISNULL(cache_dir)) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: cache_dir is null", K(ret));
    return ret;
  }
  
  if (column_metas.count() <= 0 || column_metas.count() > MAX_COLUMNS) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: invalid column count", 
             K(ret), "count", column_metas.count());
    return ret;
  }
  
  LOG_INFO("[FTS_GENERIC_CACHE] init_streaming_persist: starting", 
           K(cache_dir), "column_count", column_metas.count(), K(estimated_rows));
  
  // 1. 创建目录
  if (OB_FAIL(share::ObIODeviceLocalFileOp::mkdir(cache_dir, 0755))) {
    if (OB_FILE_OR_DIRECTORY_EXIST == ret) {
      ret = common::OB_SUCCESS;
    } else {
      LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to mkdir", K(ret), K(cache_dir));
      return ret;
    }
  }
  
  // 2. 构建文件路径
  char file_path[512];
  int n = snprintf(file_path, sizeof(file_path), "%s/fts_scalar_cache.dat", cache_dir);
  if (n < 0 || n >= static_cast<int>(sizeof(file_path))) {
    ret = common::OB_SIZE_OVERFLOW;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: path too long", K(ret));
    return ret;
  }
  
  // 3. 打开文件
  if (OB_FAIL(share::ObIODeviceLocalFileOp::open(file_path, 
                                          O_CREAT | O_WRONLY | O_TRUNC, 
                                          0644, streaming_fd_))) {
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to open file", K(ret), K(file_path));
    return ret;
  }
  
  // 4. 复制列元数据
  column_count_ = column_metas.count();
  for (int64_t i = 0; i < column_count_; ++i) {
    column_metas_[i] = column_metas.at(i);
  }
  
  // 5. 初始化文件头
  file_header_.reset();
  file_header_.magic = FtsScalarCacheFileHeader::MAGIC;
  file_header_.version = FtsScalarCacheFileHeader::CURRENT_VERSION;
  file_header_.row_count = 0;
  file_header_.column_count = column_count_;
  file_header_.max_doc_id = 0;
  file_header_.create_timestamp = common::ObTimeUtility::current_time();
  
  // 计算各 section 的偏移
  file_header_.column_meta_offset = FtsScalarCacheFileHeader::HEADER_SIZE;
  file_header_.column_meta_size = column_count_ * sizeof(FtsColumnMetaEntry);
  file_header_.row_index_offset = file_header_.column_meta_offset + file_header_.column_meta_size;
  // row_index_size 在 finish_streaming_persist 中更新
  // row_data_offset 在 finish_streaming_persist 中更新
  
  // 6. 写入初始 Header（占位）
  int64_t write_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          streaming_fd_.second_id_, &file_header_, sizeof(file_header_), 0, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to write header", K(ret));
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    return ret;
  }
  
  // 7. 写入 Column Meta
  FtsColumnMetaEntry *entries = static_cast<FtsColumnMetaEntry*>(
      ob_malloc(sizeof(FtsColumnMetaEntry) * column_count_, mem_attr_));
  if (OB_ISNULL(entries)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to alloc column meta", K(ret));
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    return ret;
  }
  
  for (int64_t i = 0; i < column_count_; ++i) {
    entries[i].column_type = static_cast<int32_t>(column_metas_[i].obj_type_);
    entries[i].type_order = static_cast<int32_t>(column_metas_[i].type_order_);
    entries[i].type_signature = column_metas_[i].type_signature_;
    entries[i].is_primary_key = column_metas_[i].is_primary_key_ ? 1 : 0;
    entries[i].is_nullable = column_metas_[i].is_nullable_ ? 1 : 0;
  }
  
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          streaming_fd_.second_id_, entries, sizeof(FtsColumnMetaEntry) * column_count_,
          file_header_.column_meta_offset, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to write column meta", K(ret));
    ob_free(entries);
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    return ret;
  }
  ob_free(entries);
  
  // 8. 分配行索引缓冲区
  streaming_row_index_capacity_ = estimated_rows > 0 ? estimated_rows : DEFAULT_CAPACITY;
  streaming_row_index_ = static_cast<FtsRowIndexEntry*>(
      ob_malloc(sizeof(FtsRowIndexEntry) * streaming_row_index_capacity_, mem_attr_));
  if (OB_ISNULL(streaming_row_index_)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to alloc row index", K(ret));
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    return ret;
  }
  memset(streaming_row_index_, 0, sizeof(FtsRowIndexEntry) * streaming_row_index_capacity_);
  
  // 9. 分配行数据序列化缓冲区
  streaming_row_buf_ = static_cast<char*>(ob_malloc(STREAMING_ROW_BUF_SIZE, mem_attr_));
  if (OB_ISNULL(streaming_row_buf_)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_GENERIC_CACHE] init_streaming_persist: fail to alloc row buffer", K(ret));
    ob_free(streaming_row_index_);
    streaming_row_index_ = nullptr;
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
    return ret;
  }
  
  // 10. 初始化状态
  // Row Data 起始位置 = Row Index 结束位置（预留空间，实际在 finish 时写入）
  // 固定 Row Data 的起始位置，即使后续 Row Index 扩容也不会改变
  streaming_row_data_start_ = file_header_.row_index_offset + 
                              streaming_row_index_capacity_ * sizeof(FtsRowIndexEntry);
  streaming_data_offset_ = 0;  // 相对于 Row Data 区域的偏移
  count_ = 0;
  streaming_mode_ = true;
  MEMCPY(cache_file_path_, file_path, strlen(file_path) + 1);
  
  LOG_INFO("[FTS_GENERIC_CACHE] init_streaming_persist: completed successfully", 
           K(file_path), K_(column_count), K_(streaming_row_index_capacity),
           K_(streaming_row_data_start));
  
  return ret;
}

int ObFtsGenericCache::serialize_row_from_values(const ObFtsColumnValue *column_values,
                                                  char *buf, int64_t buf_len, int64_t &row_size)
{
  int ret = common::OB_SUCCESS;
  char *p = buf;
  char *end = buf + buf_len;
  
  for (int64_t col_idx = 0; col_idx < column_count_ && p < end; ++col_idx) {
    const ObFtsColumnValue &val = column_values[col_idx];
    
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

int ObFtsGenericCache::streaming_add_row(uint64_t doc_id, const ObFtsColumnValue *column_values)
{
  int ret = common::OB_SUCCESS;
  
  if (!streaming_mode_) {
    ret = common::OB_NOT_INIT;
    LOG_WARN("[FTS_GENERIC_CACHE] streaming_add_row: not in streaming mode", K(ret));
    return ret;
  }
  
  if (doc_id == 0 || OB_ISNULL(column_values)) {
    ret = common::OB_INVALID_ARGUMENT;
    LOG_WARN("[FTS_GENERIC_CACHE] streaming_add_row: invalid argument", K(ret), K(doc_id));
    return ret;
  }
  
  common::ObSpinLockGuard guard(lock_);
  
  // 确保行索引缓冲区容量足够
  if (doc_id > static_cast<uint64_t>(streaming_row_index_capacity_)) {
    int64_t new_capacity = streaming_row_index_capacity_ * 2;
    while (new_capacity < static_cast<int64_t>(doc_id)) {
      new_capacity *= 2;
    }
    
    FtsRowIndexEntry *new_index = static_cast<FtsRowIndexEntry*>(
        ob_malloc(sizeof(FtsRowIndexEntry) * new_capacity, mem_attr_));
    if (OB_ISNULL(new_index)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("[FTS_GENERIC_CACHE] streaming_add_row: fail to expand row index", K(ret));
      return ret;
    }
    
    memset(new_index, 0, sizeof(FtsRowIndexEntry) * new_capacity);
    if (OB_NOT_NULL(streaming_row_index_)) {
      memcpy(new_index, streaming_row_index_, 
             sizeof(FtsRowIndexEntry) * streaming_row_index_capacity_);
      ob_free(streaming_row_index_);
    }
    streaming_row_index_ = new_index;
    streaming_row_index_capacity_ = new_capacity;
    
    LOG_INFO("[FTS_GENERIC_CACHE] streaming_add_row: expanded row index capacity", 
             K(new_capacity));
  }
  
  // 序列化行数据
  int64_t row_size = 0;
  if (OB_FAIL(serialize_row_from_values(column_values, streaming_row_buf_, 
                                        STREAMING_ROW_BUF_SIZE, row_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] streaming_add_row: fail to serialize row", K(ret), K(doc_id));
    return ret;
  }
  
  // 记录行索引
  uint64_t row_idx = doc_id - 1;
  streaming_row_index_[row_idx].data_offset = streaming_data_offset_;
  streaming_row_index_[row_idx].data_size = row_size;
  
  // 写入行数据到固定的 Row Data 区域
  // 使用初始化时确定的 streaming_row_data_start_，避免扩容时位置跳变
  int64_t write_offset = streaming_row_data_start_ + streaming_data_offset_;
  int64_t write_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          streaming_fd_.second_id_, streaming_row_buf_, row_size, write_offset, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] streaming_add_row: fail to write row", K(ret), K(doc_id));
    return ret;
  }
  
  streaming_data_offset_ += row_size;
  
  if (doc_id > count_) {
    count_ = doc_id;
  }
  
  // 采样日志
  if (count_ == 1 || count_ % 50000 == 0) {
    LOG_INFO("[FTS_GENERIC_CACHE] streaming_add_row: progress", 
             K_(count), K_(streaming_data_offset));
  }
  
  return ret;
}

int ObFtsGenericCache::streaming_add_row_from_exprs(uint64_t doc_id,
                                                     const common::ObIArray<ObExpr*> &output_exprs,
                                                     ObEvalCtx &eval_ctx)
{
  int ret = common::OB_SUCCESS;
  
  if (!streaming_mode_) {
    ret = common::OB_NOT_INIT;
    return ret;
  }
  
  if (doc_id == 0) {
    ret = common::OB_INVALID_ARGUMENT;
    return ret;
  }
  
  // 从表达式提取列值到临时数组
  ObFtsColumnValue temp_values[MAX_COLUMNS];
  
  for (int64_t col_idx = 0; col_idx < column_count_; ++col_idx) {
    const ObFtsColumnMeta &meta = column_metas_[col_idx];
    
    if (meta.output_idx_ < 0 || meta.output_idx_ >= output_exprs.count()) {
      temp_values[col_idx] = ObFtsColumnValue::make_null();
      continue;
    }
    
    ObExpr *expr = output_exprs.at(meta.output_idx_);
    if (OB_ISNULL(expr)) {
      temp_values[col_idx] = ObFtsColumnValue::make_null();
      continue;
    }
    
    ObDatum *datum = nullptr;
    int eval_ret = expr->eval(eval_ctx, datum);
    if (OB_SUCCESS != eval_ret) {
      temp_values[col_idx] = ObFtsColumnValue::make_null();
      continue;
    }
    
    if (OB_ISNULL(datum) || datum->is_null()) {
      temp_values[col_idx] = ObFtsColumnValue::make_null();
    } else {
      // 注意：流式模式下不需要深拷贝字符串，因为会立即序列化到文件
      int conv_ret = ObFtsColumnValue::from_datum(*datum, meta.obj_type_, allocator_, temp_values[col_idx]);
      if (OB_SUCCESS != conv_ret) {
        temp_values[col_idx] = ObFtsColumnValue::make_null();
      }
    }
  }
  
  // 调用 streaming_add_row 写入
  ret = streaming_add_row(doc_id, temp_values);
  
  return ret;
}

int ObFtsGenericCache::finish_streaming_persist()
{
  int ret = common::OB_SUCCESS;
  
  if (!streaming_mode_) {
    ret = common::OB_NOT_INIT;
    LOG_WARN("[FTS_GENERIC_CACHE] finish_streaming_persist: not in streaming mode", K(ret));
    return ret;
  }
  
  LOG_INFO("[FTS_GENERIC_CACHE] finish_streaming_persist: starting", 
           K_(count), K_(streaming_data_offset));
  
  common::ObSpinLockGuard guard(lock_);
  
  // 1. 更新文件头
  // 使用初始化时确定的固定起始位置，确保与写入时一致
  file_header_.row_count = count_;
  file_header_.max_doc_id = count_;
  file_header_.row_index_size = count_ * sizeof(FtsRowIndexEntry);
  file_header_.row_data_offset = streaming_row_data_start_;  // 使用固定的起始位置
  file_header_.row_data_size = streaming_data_offset_;
  
  // 2. 写入 Row Index
  int64_t write_size = 0;
  if (count_ > 0 && OB_NOT_NULL(streaming_row_index_)) {
    if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
            streaming_fd_.second_id_, streaming_row_index_, 
            count_ * sizeof(FtsRowIndexEntry),
            file_header_.row_index_offset, write_size))) {
      LOG_WARN("[FTS_GENERIC_CACHE] finish_streaming_persist: fail to write row index", K(ret));
      // 继续执行，尝试更新 header
    }
  }
  
  // 3. 更新 Header
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pwrite_impl(
          streaming_fd_.second_id_, &file_header_, sizeof(file_header_), 0, write_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] finish_streaming_persist: fail to update header", K(ret));
  }
  
  // 4. 同步并关闭文件
  if (streaming_fd_.is_normal_file()) {
    share::ObIODeviceLocalFileOp::fsync(streaming_fd_);
    share::ObIODeviceLocalFileOp::close(streaming_fd_);
  }
  
  // 5. 释放流式存储缓冲区
  if (OB_NOT_NULL(streaming_row_index_)) {
    ob_free(streaming_row_index_);
    streaming_row_index_ = nullptr;
    streaming_row_index_capacity_ = 0;
  }
  if (OB_NOT_NULL(streaming_row_buf_)) {
    ob_free(streaming_row_buf_);
    streaming_row_buf_ = nullptr;
  }
  
  // 6. 更新状态
  streaming_mode_ = false;
  streaming_data_offset_ = 0;
  // 注意：不设置 is_inited_ = true，因为内存中没有数据
  // 后续使用应该通过 KVCache 模式访问
  
  LOG_INFO("[FTS_GENERIC_CACHE] finish_streaming_persist: completed successfully",
           K_(cache_file_path), K_(count), K_(column_count),
           "row_data_size", file_header_.row_data_size);
  
  return ret;
}

// ========================================================================
// V4: HashMap 缓存实现
// ========================================================================

int ObFtsGenericCache::init_hashmap()
{
  int ret = common::OB_SUCCESS;
  
  if (hashmap_inited_) {
    return common::OB_SUCCESS;
  }
  
  common::ObSpinLockGuard guard(hashmap_lock_);
  
  // Double check
  if (hashmap_inited_) {
    return common::OB_SUCCESS;
  }
  
  // 预分配足够的 bucket 避免 rehash（1.5倍行数）
  int64_t bucket_count = file_header_.row_count > 0 ? 
      static_cast<int64_t>(file_header_.row_count * 1.5) : 10000;
  if (OB_FAIL(row_hashmap_.create(bucket_count, "FtsRowHashMap", "FtsRowHashMap"))) {
    LOG_WARN("[FTS_GENERIC_CACHE] init_hashmap: 创建 hashmap 失败", K(ret), K(bucket_count));
    return ret;
  }
  
  hashmap_inited_ = true;
  LOG_INFO("[FTS_GENERIC_CACHE] init_hashmap: hashmap 初始化完成", K(bucket_count));
  
  return ret;
}

int ObFtsGenericCache::get_row_from_hashmap(uint64_t doc_id, const ObFtsRowData *&row_data)
{
  int ret = common::OB_SUCCESS;
  row_data = nullptr;
  
  // 确保 HashMap 已初始化
  if (!hashmap_inited_) {
    if (OB_FAIL(init_hashmap())) {
      return ret;
    }
  }
  
  // 读不加锁（查询是单线程的）
  ObFtsRowData *found_row = nullptr;
  ret = row_hashmap_.get_refactored(doc_id, found_row);
  
  if (common::OB_SUCCESS == ret && OB_NOT_NULL(found_row)) {
    row_data = found_row;
    return common::OB_SUCCESS;
  }
  
  // HashMap 未命中，从磁盘加载
  if (common::OB_HASH_NOT_EXIST == ret) {
    ret = load_row_to_hashmap(doc_id, row_data);
  }
  
  return ret;
}

int ObFtsGenericCache::load_row_to_hashmap(uint64_t doc_id, const ObFtsRowData *&row_data)
{
  int ret = common::OB_SUCCESS;
  row_data = nullptr;
  
  // 检查文件是否已打开
  if (!persist_file_opened_) {
    ret = ensure_ready();
    if (OB_FAIL(ret)) {
      return ret;
    }
  }
  
  // 检查 doc_id 有效性
  if (doc_id == 0 || doc_id > static_cast<uint64_t>(file_header_.row_count)) {
    return common::OB_ENTRY_NOT_EXIST;
  }
  
  // 检查行索引
  if (OB_ISNULL(row_index_)) {
    return common::OB_NOT_INIT;
  }
  
  const FtsRowIndexEntry &index_entry = row_index_[doc_id - 1];
  if (index_entry.data_size == 0) {
    return common::OB_ENTRY_NOT_EXIST;
  }
  
  // 1. 使用临时 buffer 读取磁盘数据（不加锁，IO 耗时）
  char *temp_buf = static_cast<char*>(ob_malloc(index_entry.data_size, "FtsTmpRow"));
  if (OB_ISNULL(temp_buf)) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_hashmap: 分配临时 buffer 失败", K(ret));
    return common::OB_ALLOCATE_MEMORY_FAILED;
  }
  
  int64_t data_offset = file_header_.row_data_offset + index_entry.data_offset;
  int64_t read_size = 0;
  if (OB_FAIL(share::ObIODeviceLocalFileOp::pread_impl(
          persist_fd_.second_id_, temp_buf, index_entry.data_size,
          data_offset, read_size))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_hashmap: 读取磁盘失败", K(ret), K(doc_id));
    ob_free(temp_buf);
    return ret;
  }
  
  // 2. 加锁进行内存分配和反序列化
  common::ObSpinLockGuard guard(hashmap_lock_);
  
  // Double check: 可能其他线程已经加载了
  ObFtsRowData *existing_row = nullptr;
  if (common::OB_SUCCESS == row_hashmap_.get_refactored(doc_id, existing_row)) {
    row_data = existing_row;
    ob_free(temp_buf);
    return common::OB_SUCCESS;
  }
  
  // 3. 分配 ObFtsRowData
  ObFtsRowData *new_row = static_cast<ObFtsRowData*>(
      hashmap_allocator_.alloc(sizeof(ObFtsRowData)));
  if (OB_ISNULL(new_row)) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_hashmap: 分配 ObFtsRowData 失败", K(ret));
    ob_free(temp_buf);
    return common::OB_ALLOCATE_MEMORY_FAILED;
  }
  new (new_row) ObFtsRowData();
  
  // 4. 初始化并反序列化
  if (OB_FAIL(new_row->init(file_header_.column_count, hashmap_allocator_))) {
    LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_hashmap: 初始化行失败", K(ret));
    ob_free(temp_buf);
    return ret;
  }
  
  // 反序列化（格式与 KVCache 相同）
  const char *p = temp_buf;
  const char *end = temp_buf + index_entry.data_size;
  
  for (int64_t col = 0; col < file_header_.column_count && p < end; ++col) {
    ObFtsColumnValue col_val;
    
    // 读取类型标记
    col_val.type_ = static_cast<ObFtsColumnValue::Type>(*p++);
    
    switch (col_val.type_) {
      case ObFtsColumnValue::TYPE_NULL:
        // 无数据
        break;
      case ObFtsColumnValue::TYPE_INT64:
        if (p + sizeof(int64_t) <= end) {
          memcpy(&col_val.int_val_, p, sizeof(int64_t));
          p += sizeof(int64_t);
        }
        break;
      case ObFtsColumnValue::TYPE_UINT64:
        if (p + sizeof(uint64_t) <= end) {
          memcpy(&col_val.uint_val_, p, sizeof(uint64_t));
          p += sizeof(uint64_t);
        }
        break;
      case ObFtsColumnValue::TYPE_DOUBLE:
        if (p + sizeof(double) <= end) {
          memcpy(&col_val.double_val_, p, sizeof(double));
          p += sizeof(double);
        }
        break;
      case ObFtsColumnValue::TYPE_STRING: {
        if (p + sizeof(int32_t) <= end) {
          int32_t len = 0;
          memcpy(&len, p, sizeof(int32_t));
          p += sizeof(int32_t);
          if (len > 0 && p + len <= end) {
            // 深拷贝字符串到 hashmap_allocator_
            char *str_buf = static_cast<char*>(hashmap_allocator_.alloc(len));
            if (OB_NOT_NULL(str_buf)) {
              memcpy(str_buf, p, len);
              col_val.str_val_.ptr_ = str_buf;
              col_val.str_val_.len_ = len;
            }
            p += len;
          }
        }
        break;
      }
      case ObFtsColumnValue::TYPE_DATETIME:
        if (p + sizeof(int64_t) <= end) {
          memcpy(&col_val.datetime_val_, p, sizeof(int64_t));
          p += sizeof(int64_t);
        }
        break;
      default:
        col_val.type_ = ObFtsColumnValue::TYPE_NULL;
        break;
    }
    
    new_row->set_column(col, col_val);
  }
  
  // 5. 放入 HashMap
  if (OB_FAIL(row_hashmap_.set_refactored(doc_id, new_row))) {
    if (common::OB_HASH_EXIST == ret) {
      // 已存在（并发场景），获取已有的
      ret = common::OB_SUCCESS;
      if (OB_SUCC(row_hashmap_.get_refactored(doc_id, existing_row))) {
        row_data = existing_row;
      }
    } else {
      LOG_WARN("[FTS_GENERIC_CACHE] load_row_to_hashmap: 插入 hashmap 失败", K(ret), K(doc_id));
    }
  } else {
    row_data = new_row;
  }
  
  // 6. 释放临时 buffer
  ob_free(temp_buf);
  
  return ret;
}

} // namespace sql
} // namespace oceanbase
