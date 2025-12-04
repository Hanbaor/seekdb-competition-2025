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
    allocator_("FtsScalarCache"),
    mapping_()
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
  } else if (OB_FAIL(mapping_.create(capacity, "FtsScalarMap"))) {
    LOG_WARN("failed to create hash map", K(ret), K(capacity));
  } else {
    is_inited_ = true;
    LOG_INFO("fts scalar cache initialized", K(capacity));
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
  } else {
    ScalarFields fields;
    fields.id_ = id;
    
    // 复制 base_id 字符串到 allocator
    if (base_id.length() > 0) {
      char *buf = static_cast<char*>(allocator_.alloc(base_id.length()));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate memory for base_id", K(ret), K(base_id.length()));
      } else {
        MEMCPY(buf, base_id.ptr(), base_id.length());
        fields.base_id_.assign_ptr(buf, base_id.length());
      }
    }
    
    // 复制 docid_col 字符串到 allocator
    if (OB_SUCC(ret) && docid_col.length() > 0) {
      char *buf = static_cast<char*>(allocator_.alloc(docid_col.length()));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate memory for docid_col", K(ret), K(docid_col.length()));
      } else {
        MEMCPY(buf, docid_col.ptr(), docid_col.length());
        fields.docid_col_.assign_ptr(buf, docid_col.length());
      }
    }
    
    // 插入到 HashMap（使用覆盖模式，避免重复插入失败）
    if (OB_SUCC(ret)) {
      if (OB_FAIL(mapping_.set_refactored(doc_id, fields, 1 /*overwrite*/))) {
        LOG_WARN("failed to add mapping to hash map", K(ret), K(doc_id), K(fields));
      } else {
        LOG_DEBUG("added mapping", K(doc_id), K(fields));
      }
    }
  }
  
  return ret;
}

int ObFtsScalarCache::get_scalar_fields(uint64_t doc_id, ScalarFields &fields)
{
  int ret = OB_SUCCESS;
  
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("fts scalar cache not init", K(ret));
  } else if (OB_FAIL(mapping_.get_refactored(doc_id, fields))) {
    if (OB_HASH_NOT_EXIST != ret) {
      LOG_WARN("failed to get scalar fields", K(ret), K(doc_id));
    }
  } else {
    LOG_DEBUG("got scalar fields", K(doc_id), K(fields));
  }
  
  return ret;
}

int64_t ObFtsScalarCache::size() const
{
  return is_inited_ ? mapping_.size() : 0;
}

void ObFtsScalarCache::destroy()
{
  if (is_inited_) {
    mapping_.destroy();
    allocator_.reset();
    is_inited_ = false;
    LOG_INFO("fts scalar cache destroyed");
  }
}

} // namespace sql
} // namespace oceanbase
