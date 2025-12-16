/*
 * Copyright (c) 2025 OceanBase.
 *
 * FTS Scalar KVCache 实现
 * V2 版本：全局缓存管理和列值访问函数
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_scalar_kvcache.h"
#include "lib/allocator/ob_malloc.h"

namespace oceanbase
{
namespace sql
{

// ========================================================================
// 全局 KVCache 实例管理
// ========================================================================

static ObFtsScalarKVCache *g_scalar_kvcache = nullptr;
static bool g_scalar_kvcache_inited = false;

int init_fts_scalar_kvcache()
{
  int ret = OB_SUCCESS;
  
  if (g_scalar_kvcache_inited) {
    LOG_INFO("[FTS_SCALAR_KVCACHE] already initialized");
    return OB_SUCCESS;
  }
  
  LOG_INFO("[FTS_SCALAR_KVCACHE] initializing ScalarKVCache");
  
  void *buf = ob_malloc(sizeof(ObFtsScalarKVCache), "FtsScalarKV");
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("[FTS_SCALAR_KVCACHE] fail to alloc ScalarKVCache", K(ret));
    return ret;
  }
  
  g_scalar_kvcache = new (buf) ObFtsScalarKVCache();
  
  // 初始化缓存（参数：cache_name, priority）
  if (OB_FAIL(g_scalar_kvcache->init("fts_scalar_cache", 1))) {
    LOG_WARN("[FTS_SCALAR_KVCACHE] fail to init ScalarKVCache", K(ret));
    g_scalar_kvcache->~ObFtsScalarKVCache();
    ob_free(buf);
    g_scalar_kvcache = nullptr;
    return ret;
  }
  
  g_scalar_kvcache_inited = true;
  LOG_INFO("[FTS_SCALAR_KVCACHE] ScalarKVCache initialized successfully");
  
  return ret;
}

void destroy_fts_scalar_kvcache()
{
  LOG_INFO("[FTS_SCALAR_KVCACHE] destroying ScalarKVCache");
  
  if (g_scalar_kvcache) {
    g_scalar_kvcache->destroy();
    g_scalar_kvcache->~ObFtsScalarKVCache();
    ob_free(g_scalar_kvcache);
    g_scalar_kvcache = nullptr;
  }
  g_scalar_kvcache_inited = false;
  
  LOG_INFO("[FTS_SCALAR_KVCACHE] ScalarKVCache destroyed");
}

bool is_fts_scalar_kvcache_inited()
{
  return g_scalar_kvcache_inited;
}

ObFtsScalarKVCache* get_fts_scalar_kvcache()
{
  return g_scalar_kvcache;
}

// ========================================================================
// ObFtsScalarCacheValue 列值访问函数实现
// ========================================================================

int ObFtsScalarCacheValue::get_column_value(int64_t col_idx, ObFtsColumnValue &out_value) const
{
  int ret = OB_SUCCESS;
  
  if (col_idx < 0 || col_idx >= column_count_) {
    ret = OB_INVALID_ARGUMENT;
    return ret;
  }
  
  if (OB_ISNULL(data_) || data_size_ <= 0) {
    out_value = ObFtsColumnValue::make_null();
    return ret;
  }
  
  // 遍历解析到指定列
  const char *p = data_;
  const char *end = data_ + data_size_;
  
  for (int64_t i = 0; i <= col_idx && p < end; ++i) {
    // 读取类型标记
    ObFtsColumnValue::Type type = static_cast<ObFtsColumnValue::Type>(*p++);
    
    if (i == col_idx) {
      // 找到目标列
      out_value.type_ = type;
      
      switch (type) {
        case ObFtsColumnValue::TYPE_NULL:
          // 无数据
          break;
        case ObFtsColumnValue::TYPE_INT64:
          if (p + sizeof(int64_t) <= end) {
            memcpy(&out_value.int_val_, p, sizeof(int64_t));
          }
          break;
        case ObFtsColumnValue::TYPE_UINT64:
          if (p + sizeof(uint64_t) <= end) {
            memcpy(&out_value.uint_val_, p, sizeof(uint64_t));
          }
          break;
        case ObFtsColumnValue::TYPE_DOUBLE:
          if (p + sizeof(double) <= end) {
            memcpy(&out_value.double_val_, p, sizeof(double));
          }
          break;
        case ObFtsColumnValue::TYPE_STRING:
          if (p + sizeof(int32_t) <= end) {
            int32_t len = 0;
            memcpy(&len, p, sizeof(int32_t));
            p += sizeof(int32_t);
            if (len > 0 && p + len <= end) {
              out_value.str_val_.ptr_ = const_cast<char*>(p);
              out_value.str_val_.len_ = len;
            } else {
              out_value.str_val_.ptr_ = nullptr;
              out_value.str_val_.len_ = 0;
            }
          }
          break;
        case ObFtsColumnValue::TYPE_DATETIME:
          if (p + sizeof(int64_t) <= end) {
            memcpy(&out_value.datetime_val_, p, sizeof(int64_t));
          }
          break;
        default:
          out_value.type_ = ObFtsColumnValue::TYPE_NULL;
          break;
      }
      return ret;
    }
    
    // 跳过当前列的数据
    switch (type) {
      case ObFtsColumnValue::TYPE_NULL:
        break;
      case ObFtsColumnValue::TYPE_INT64:
      case ObFtsColumnValue::TYPE_UINT64:
      case ObFtsColumnValue::TYPE_DOUBLE:
      case ObFtsColumnValue::TYPE_DATETIME:
        p += sizeof(int64_t);
        break;
      case ObFtsColumnValue::TYPE_STRING: {
        if (p + sizeof(int32_t) <= end) {
          int32_t len = 0;
          memcpy(&len, p, sizeof(int32_t));
          p += sizeof(int32_t) + len;
        }
        break;
      }
      default:
        break;
    }
  }
  
  // 未找到指定列
  out_value = ObFtsColumnValue::make_null();
  return ret;
}

int64_t ObFtsScalarCacheValue::get_int_column_fast(int64_t col_idx) const
{
  ObFtsColumnValue val;
  if (OB_SUCCESS == get_column_value(col_idx, val)) {
    if (val.type_ == ObFtsColumnValue::TYPE_INT64) {
      return val.int_val_;
    } else if (val.type_ == ObFtsColumnValue::TYPE_UINT64) {
      return static_cast<int64_t>(val.uint_val_);
    }
  }
  return 0;
}

common::ObString ObFtsScalarCacheValue::get_string_column_fast(int64_t col_idx) const
{
  ObFtsColumnValue val;
  if (OB_SUCCESS == get_column_value(col_idx, val)) {
    if (val.type_ == ObFtsColumnValue::TYPE_STRING) {
      return common::ObString(val.str_val_.len_, val.str_val_.ptr_);
    }
  }
  return common::ObString();
}

} // namespace sql
} // namespace oceanbase

