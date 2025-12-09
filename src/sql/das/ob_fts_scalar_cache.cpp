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

// constexpr 静态成员需要类外定义
constexpr int64_t ObFtsScalarCache::DEFAULT_CAPACITY;

ObFtsScalarCache::ObFtsScalarCache()
  : is_inited_(false),
    count_(0),
    capacity_(0),
    mem_attr_(common::OB_SERVER_TENANT_ID, "FtsScalarCache"),
    data_(nullptr),
    valid_(nullptr)
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
    // 分配动态数组
    data_ = static_cast<ScalarFields*>(ob_malloc(sizeof(ScalarFields) * capacity, mem_attr_));
    valid_ = static_cast<bool*>(ob_malloc(sizeof(bool) * capacity, mem_attr_));
    
    if (OB_ISNULL(data_) || OB_ISNULL(valid_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory", K(ret), K(capacity));
      if (data_) { ob_free(data_); data_ = nullptr; }
      if (valid_) { ob_free(valid_); valid_ = nullptr; }
    } else {
      // 初始化数组
      for (int64_t i = 0; i < capacity; ++i) {
        new (&data_[i]) ScalarFields();
        valid_[i] = false;
      }
      capacity_ = capacity;
      count_ = 0;
      is_inited_ = true;
      LOG_INFO("fts scalar cache initialized", K(capacity));
    }
  }
  
  return ret;
}

int ObFtsScalarCache::ensure_capacity(uint64_t doc_id)
{
  int ret = OB_SUCCESS;
  
  if (doc_id < static_cast<uint64_t>(capacity_)) {
    // 已经有足够容量
  } else {
    // 需要扩容：至少翻倍，且能容纳 doc_id
    int64_t new_capacity = capacity_ * 2;
    while (new_capacity <= static_cast<int64_t>(doc_id)) {
      new_capacity *= 2;
    }
    
    ScalarFields *new_data = static_cast<ScalarFields*>(ob_malloc(sizeof(ScalarFields) * new_capacity, mem_attr_));
    bool *new_valid = static_cast<bool*>(ob_malloc(sizeof(bool) * new_capacity, mem_attr_));
    
    if (OB_ISNULL(new_data) || OB_ISNULL(new_valid)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory for resize", K(ret), K(new_capacity));
      if (new_data) ob_free(new_data);
      if (new_valid) ob_free(new_valid);
    } else {
      // 直接移动旧数据（不是深拷贝，只移动指针）
      MEMCPY(new_data, data_, sizeof(ScalarFields) * capacity_);
      MEMCPY(new_valid, valid_, sizeof(bool) * capacity_);
      
      // 初始化新增部分
      for (int64_t i = capacity_; i < new_capacity; ++i) {
        new (&new_data[i]) ScalarFields();
        new_valid[i] = false;
      }
      
      // 释放旧数组结构（字符串指针已转移，不要释放字符串内存）
      ob_free(data_);
      ob_free(valid_);
      
      data_ = new_data;
      valid_ = new_valid;
      capacity_ = new_capacity;
      
      LOG_INFO("fts scalar cache resized", K(new_capacity));
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
  } else if (OB_FAIL(ensure_capacity(doc_id))) {
    LOG_WARN("failed to ensure capacity", K(ret), K(doc_id));
  } else {
    data_[doc_id].id_ = id;
    
    // 复制 base_id 字符串
    if (base_id.length() > 0) {
      char *buf = static_cast<char*>(ob_malloc(base_id.length(), mem_attr_));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, base_id.ptr(), base_id.length());
        data_[doc_id].base_id_.assign_ptr(buf, base_id.length());
      }
    }
    
    // 复制 docid_col 字符串
    if (OB_SUCC(ret) && docid_col.length() > 0) {
      char *buf = static_cast<char*>(ob_malloc(docid_col.length(), mem_attr_));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, docid_col.ptr(), docid_col.length());
        data_[doc_id].docid_col_.assign_ptr(buf, docid_col.length());
      }
    }
    
    if (OB_SUCC(ret)) {
      if (!valid_[doc_id]) {
        valid_[doc_id] = true;
        count_++;
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
  } else if (doc_id >= static_cast<uint64_t>(capacity_)) {
    ret = OB_ENTRY_NOT_EXIST;
  } else if (!valid_[doc_id]) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    fields = data_[doc_id];
  }
  
  return ret;
}

void ObFtsScalarCache::destroy()
{
  if (is_inited_) {
    // 释放所有分配的字符串内存
    for (int64_t i = 0; i < capacity_; ++i) {
      if (valid_[i]) {
        if (data_[i].base_id_.ptr() != nullptr) {
          ob_free(const_cast<char*>(data_[i].base_id_.ptr()));
        }
        if (data_[i].docid_col_.ptr() != nullptr) {
          ob_free(const_cast<char*>(data_[i].docid_col_.ptr()));
        }
      }
    }
    
    // 释放数组
    if (data_) { ob_free(data_); data_ = nullptr; }
    if (valid_) { ob_free(valid_); valid_ = nullptr; }
    
    capacity_ = 0;
    count_ = 0;
    is_inited_ = false;
    LOG_INFO("fts scalar cache destroyed");
  }
}

} // namespace sql
} // namespace oceanbase
