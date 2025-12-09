/*
 * Copyright (c) 2025 OceanBase.
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_scalar_cache.h"

namespace oceanbase
{
namespace sql
{

// 全局单例实例
ObFtsScalarCache g_fts_scalar_cache;

ObFtsScalarCache::ObFtsScalarCache()
  : is_inited_(false),
    mem_attr_(common::OB_SERVER_TENANT_ID, "FtsScalarCache"),
    data_(),
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
    // 预分配容量
    if (OB_FAIL(data_.reserve(capacity))) {
      LOG_WARN("failed to reserve capacity", K(ret), K(capacity));
    } else {
      is_inited_ = true;
      LOG_INFO("fts scalar cache initialized with ObArray", K(capacity));
    }
  }
  
  return ret;
}

int ObFtsScalarCache::add_mapping(uint64_t doc_id,
                                   uint64_t id,
                                   const common::ObString &base_id,
                                   const common::ObString &docid_col)
{
  int ret = OB_SUCCESS;
  
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("fts scalar cache not init", K(ret));
  } else if (doc_id == 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("doc_id must be >= 1", K(ret), K(doc_id));
  } else {
    common::ObSpinLockGuard guard(lock_);  // 加锁保护
    
    // 确保容量足够（doc_id 从 1 开始，需要索引 doc_id-1）
    int64_t required = static_cast<int64_t>(doc_id);
    if (data_.count() < required) {
      // 扩容到足够大小
      if (OB_FAIL(data_.prepare_allocate(required))) {
        LOG_WARN("failed to prepare allocate", K(ret), K(required));
      }
    }
    
    if (OB_SUCC(ret)) {
      int64_t idx = doc_id - 1;
      ScalarFields &fields = data_.at(idx);
      fields.id_ = id;
      
      // 复制 base_id 字符串
      if (base_id.length() > 0) {
        char *buf = static_cast<char*>(ob_malloc(base_id.length(), mem_attr_));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, base_id.ptr(), base_id.length());
          fields.base_id_.assign_ptr(buf, base_id.length());
        }
      }
      
      // 复制 docid_col 字符串
      if (OB_SUCC(ret) && docid_col.length() > 0) {
        char *buf = static_cast<char*>(ob_malloc(docid_col.length(), mem_attr_));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, docid_col.ptr(), docid_col.length());
          fields.docid_col_.assign_ptr(buf, docid_col.length());
        }
      }
    }
  }
  
  return ret;
}

int ObFtsScalarCache::get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const
{
  int ret = OB_SUCCESS;
  
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (doc_id == 0 || doc_id > static_cast<uint64_t>(data_.count())) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    fields = data_.at(doc_id - 1);  // doc_id 从 1 开始
  }
  
  return ret;
}

void ObFtsScalarCache::reuse()
{
  // 释放所有字符串内存
  for (int64_t i = 0; i < data_.count(); ++i) {
    if (data_.at(i).base_id_.ptr() != nullptr) {
      ob_free(const_cast<char*>(data_.at(i).base_id_.ptr()));
    }
    if (data_.at(i).docid_col_.ptr() != nullptr) {
      ob_free(const_cast<char*>(data_.at(i).docid_col_.ptr()));
    }
  }
  data_.reuse();
}

void ObFtsScalarCache::destroy()
{
  if (is_inited_) {
    reuse();
    data_.destroy();
    is_inited_ = false;
    LOG_INFO("fts scalar cache destroyed");
  }
}

} // namespace sql
} // namespace oceanbase
