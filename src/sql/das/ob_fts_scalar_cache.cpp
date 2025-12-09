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
    // 分别分配三个列数组
    ids_ = static_cast<uint64_t*>(ob_malloc(sizeof(uint64_t) * capacity, mem_attr_));
    base_ids_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    docid_cols_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    
    if (OB_ISNULL(ids_) || OB_ISNULL(base_ids_) || OB_ISNULL(docid_cols_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate column arrays", K(ret), K(capacity));
      // 清理已分配的
      if (ids_) { ob_free(ids_); ids_ = nullptr; }
      if (base_ids_) { ob_free(base_ids_); base_ids_ = nullptr; }
      if (docid_cols_) { ob_free(docid_cols_); docid_cols_ = nullptr; }
    } else {
      // 初始化数组
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
    // 容量足够
  } else {
    // 扩容：每次翻倍
    int64_t new_capacity = capacity_ > 0 ? capacity_ : DEFAULT_CAPACITY;
    while (new_capacity < static_cast<int64_t>(required)) {
      new_capacity *= 2;
      if (new_capacity <= 0) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("capacity overflow", K(ret), K(required));
        return ret;
      }
    }
    
    // 分配新数组
    uint64_t *new_ids = static_cast<uint64_t*>(ob_malloc(sizeof(uint64_t) * new_capacity, mem_attr_));
    common::ObString *new_base_ids = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * new_capacity, mem_attr_));
    common::ObString *new_docid_cols = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * new_capacity, mem_attr_));
    
    if (OB_ISNULL(new_ids) || OB_ISNULL(new_base_ids) || OB_ISNULL(new_docid_cols)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate new arrays", K(ret), K(new_capacity));
      if (new_ids) ob_free(new_ids);
      if (new_base_ids) ob_free(new_base_ids);
      if (new_docid_cols) ob_free(new_docid_cols);
    } else {
      // 复制旧数据
      if (count_ > 0) {
        MEMCPY(new_ids, ids_, sizeof(uint64_t) * count_);
        MEMCPY(new_base_ids, base_ids_, sizeof(common::ObString) * count_);
        MEMCPY(new_docid_cols, docid_cols_, sizeof(common::ObString) * count_);
      }
      // 初始化新增部分
      MEMSET(new_ids + count_, 0, sizeof(uint64_t) * (new_capacity - count_));
      for (int64_t i = count_; i < new_capacity; ++i) {
        new (&new_base_ids[i]) common::ObString();
        new (&new_docid_cols[i]) common::ObString();
      }
      
      // 释放旧数组（不释放字符串内容，因为已经移动）
      if (ids_) ob_free(ids_);
      if (base_ids_) ob_free(base_ids_);
      if (docid_cols_) ob_free(docid_cols_);
      
      ids_ = new_ids;
      base_ids_ = new_base_ids;
      docid_cols_ = new_docid_cols;
      capacity_ = new_capacity;
      
      LOG_INFO("fts scalar cache expanded", K(capacity_));
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
    common::ObSpinLockGuard guard(lock_);
    
    // 确保容量足够
    if (OB_FAIL(ensure_capacity(doc_id))) {
      LOG_WARN("failed to ensure capacity", K(ret), K(doc_id));
    } else {
      uint64_t idx = doc_id - 1;
      ids_[idx] = id;
      
      // 复制 base_id 字符串
      if (base_id.length() > 0) {
        char *buf = static_cast<char*>(ob_malloc(base_id.length(), mem_attr_));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, base_id.ptr(), base_id.length());
          base_ids_[idx].assign_ptr(buf, base_id.length());
        }
      }
      
      // 复制 docid_col 字符串
      if (OB_SUCC(ret) && docid_col.length() > 0) {
        char *buf = static_cast<char*>(ob_malloc(docid_col.length(), mem_attr_));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, docid_col.ptr(), docid_col.length());
          docid_cols_[idx].assign_ptr(buf, docid_col.length());
        }
      }
      
      // 更新计数
      if (OB_SUCC(ret) && doc_id > count_) {
        count_ = doc_id;
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
  } else if (doc_id == 0 || doc_id > count_) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    common::ObSpinLockGuard guard(lock_);
    uint64_t idx = doc_id - 1;
    fields.id_ = ids_[idx];
    fields.base_id_ = base_ids_[idx];
    fields.docid_col_ = docid_cols_[idx];
  }
  
  return ret;
}

void ObFtsScalarCache::reuse()
{
  common::ObSpinLockGuard guard(lock_);
  // 释放所有字符串内存
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
