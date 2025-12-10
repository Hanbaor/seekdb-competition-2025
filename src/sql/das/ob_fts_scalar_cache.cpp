/*
 * Copyright (c) 2025 OceanBase.
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_scalar_cache.h"
#include "share/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include "lib/stat/ob_diagnose_info.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <algorithm>

namespace oceanbase
{
namespace sql
{

// 调试日志（生产环境禁用）
#define FTS_DEBUG_LOG 0
static void fts_debug_log(const char *fmt, ...)
{
#if FTS_DEBUG_LOG
  static FILE *fp = nullptr;
  if (!fp) {
    fp = fopen("/tmp/fts_scalar_cache.log", "a");
  }
  if (fp) {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);
  }
#else
  UNUSED(fmt);
#endif
}

// 全局单例实例
ObFtsScalarCache g_fts_scalar_cache;

ObFtsScalarCache::ObFtsScalarCache()
  : is_inited_(false),
    mem_attr_(common::OB_SERVER_TENANT_ID, "FtsScalarCache"),
    ids_(nullptr),
    base_ids_(nullptr),
    docid_cols_(nullptr),
    count_(0),
    capacity_(0),
    lock_()
{
}

ObFtsScalarCache::~ObFtsScalarCache()
{
  destroy();
}

int ObFtsScalarCache::init(int64_t capacity)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("fts scalar cache init twice", K(ret));
  } else if (capacity <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid capacity", K(ret), K(capacity));
  } else {
    ids_ = static_cast<uint64_t*>(ob_malloc(sizeof(uint64_t) * capacity, mem_attr_));
    base_ids_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    docid_cols_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    
    if (OB_ISNULL(ids_) || OB_ISNULL(base_ids_) || OB_ISNULL(docid_cols_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate column arrays", K(ret), K(capacity));
      if (ids_) { ob_free(ids_); ids_ = nullptr; }
      if (base_ids_) { ob_free(base_ids_); base_ids_ = nullptr; }
      if (docid_cols_) { ob_free(docid_cols_); docid_cols_ = nullptr; }
    } else {
      MEMSET(ids_, 0, sizeof(uint64_t) * capacity);
      for (int64_t i = 0; i < capacity; ++i) {
        new (&base_ids_[i]) common::ObString();
        new (&docid_cols_[i]) common::ObString();
      }
      capacity_ = capacity;
      count_ = 0;
      is_inited_ = true;
      LOG_INFO("fts scalar cache initialized with columnar storage", K(capacity));
    }
  }
  return ret;
}

int ObFtsScalarCache::ensure_capacity(uint64_t required)
{
  int ret = OB_SUCCESS;
  if (static_cast<int64_t>(required) <= capacity_) {
    return OB_SUCCESS;
  }
  
  int64_t new_capacity = capacity_ > 0 ? capacity_ : DEFAULT_CAPACITY;
  while (new_capacity < static_cast<int64_t>(required)) {
    new_capacity *= 2;
  }
  
  uint64_t *new_ids = static_cast<uint64_t*>(ob_malloc(sizeof(uint64_t) * new_capacity, mem_attr_));
  common::ObString *new_base_ids = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * new_capacity, mem_attr_));
  common::ObString *new_docid_cols = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * new_capacity, mem_attr_));
  
  if (OB_ISNULL(new_ids) || OB_ISNULL(new_base_ids) || OB_ISNULL(new_docid_cols)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    if (new_ids) ob_free(new_ids);
    if (new_base_ids) ob_free(new_base_ids);
    if (new_docid_cols) ob_free(new_docid_cols);
  } else {
    if (count_ > 0) {
      MEMCPY(new_ids, ids_, sizeof(uint64_t) * count_);
      MEMCPY(new_base_ids, base_ids_, sizeof(common::ObString) * count_);
      MEMCPY(new_docid_cols, docid_cols_, sizeof(common::ObString) * count_);
    }
    MEMSET(new_ids + count_, 0, sizeof(uint64_t) * (new_capacity - count_));
    for (int64_t i = count_; i < new_capacity; ++i) {
      new (&new_base_ids[i]) common::ObString();
      new (&new_docid_cols[i]) common::ObString();
    }
    
    if (ids_) ob_free(ids_);
    if (base_ids_) ob_free(base_ids_);
    if (docid_cols_) ob_free(docid_cols_);
    
    ids_ = new_ids;
    base_ids_ = new_base_ids;
    docid_cols_ = new_docid_cols;
    capacity_ = new_capacity;
    
    LOG_INFO("fts scalar cache expanded", K(capacity_));
  }
  return ret;
}

int ObFtsScalarCache::add_mapping(uint64_t doc_id, uint64_t id,
                                   const common::ObString &base_id,
                                   const common::ObString &docid_col)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    return OB_NOT_INIT;
  }
  if (doc_id == 0) {
    return OB_INVALID_ARGUMENT;
  }
  
  common::ObSpinLockGuard guard(lock_);
  if (OB_FAIL(ensure_capacity(doc_id))) {
    LOG_WARN("failed to ensure capacity", K(ret), K(doc_id));
  } else {
    uint64_t idx = doc_id - 1;
    ids_[idx] = id;
    
    if (base_id.length() > 0) {
      char *buf = static_cast<char*>(ob_malloc(base_id.length(), mem_attr_));
      if (buf) {
        MEMCPY(buf, base_id.ptr(), base_id.length());
        base_ids_[idx].assign_ptr(buf, base_id.length());
      }
    }
    
    if (docid_col.length() > 0) {
      char *buf = static_cast<char*>(ob_malloc(docid_col.length(), mem_attr_));
      if (buf) {
        MEMCPY(buf, docid_col.ptr(), docid_col.length());
        docid_cols_[idx].assign_ptr(buf, docid_col.length());
      }
    }
    
    if (doc_id > count_) {
      count_ = doc_id;
    }
  }
  return ret;
}

int ObFtsScalarCache::get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    return OB_NOT_INIT;
  }
  if (doc_id == 0 || doc_id > count_) {
    return OB_ENTRY_NOT_EXIST;
  }
  
  common::ObSpinLockGuard guard(lock_);
  uint64_t idx = doc_id - 1;
  fields.id_ = ids_[idx];
  fields.base_id_ = base_ids_[idx];
  fields.docid_col_ = docid_cols_[idx];
  
  return ret;
}

void ObFtsScalarCache::reuse()
{
  common::ObSpinLockGuard guard(lock_);
  for (uint64_t i = 0; i < count_; ++i) {
    if (base_ids_[i].ptr() != nullptr) {
      ob_free(const_cast<char*>(base_ids_[i].ptr()));
      base_ids_[i].reset();
    }
    if (docid_cols_[i].ptr() != nullptr) {
      ob_free(const_cast<char*>(docid_cols_[i].ptr()));
      docid_cols_[i].reset();
    }
    ids_[i] = 0;
  }
  count_ = 0;
}

void ObFtsScalarCache::destroy()
{
  if (is_inited_) {
    reuse();
    if (ids_) { ob_free(ids_); ids_ = nullptr; }
    if (base_ids_) { ob_free(base_ids_); base_ids_ = nullptr; }
    if (docid_cols_) { ob_free(docid_cols_); docid_cols_ = nullptr; }
    capacity_ = 0;
    is_inited_ = false;
    LOG_INFO("fts scalar cache destroyed");
  }
}

} // namespace sql
} // namespace oceanbase
