/*
 * Copyright (c) 2025 OceanBase.
 * 
 * 临时的初始化工具：手工填充 FTS Scalar Cache
 * 用法：在查询前调用 init_fts_scalar_cache_for_items1()
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_scalar_cache.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace sql
{

// 临时函数：为 items1 表手工填充 HashMap
// 这个函数模拟了索引构建时应该做的事情
int init_fts_scalar_cache_for_items1()
{
  int ret = OB_SUCCESS;
  
  LOG_INFO("=== Starting FTS Scalar Cache initialization for items1 ===");
  
  // 1. 初始化 HashMap
  if (OB_FAIL(g_fts_scalar_cache.init(200000))) {
    LOG_WARN("failed to init fts scalar cache", K(ret));
    return ret;
  }
  
  LOG_INFO("HashMap initialized with capacity 1000000");
  
  // 2. 手工添加测试数据
  // 这里需要根据实际的 fts_doc_rowkey 表和 items1 表来填充
  // 示例：假设有以下映射关系
  
  // doc_id=100 → items1.id=1
  if (OB_SUCC(ret)) {
    ret = g_fts_scalar_cache.add_mapping(
      100,                              // doc_id
      1,                                // id
      common::ObString("base_1"),       // base_id
      common::ObString("doc_001")       // docid_col
    );
    if (OB_SUCC(ret)) {
      LOG_INFO("Added mapping: doc_id=100 -> id=1, base_id=base_1, docid_col=doc_001");
    }
  }
  
  // doc_id=200 → items1.id=5
  if (OB_SUCC(ret)) {
    ret = g_fts_scalar_cache.add_mapping(
      200,
      5,
      common::ObString("base_2"),
      common::ObString("doc_005")
    );
    if (OB_SUCC(ret)) {
      LOG_INFO("Added mapping: doc_id=200 -> id=5, base_id=base_2, docid_col=doc_005");
    }
  }
  
  // doc_id=300 → items1.id=7
  if (OB_SUCC(ret)) {
    ret = g_fts_scalar_cache.add_mapping(
      300,
      7,
      common::ObString("base_1"),
      common::ObString("doc_007")
    );
    if (OB_SUCC(ret)) {
      LOG_INFO("Added mapping: doc_id=300 -> id=7, base_id=base_1, docid_col=doc_007");
    }
  }
  
  if (OB_SUCC(ret)) {
    LOG_INFO("=== FTS Scalar Cache initialization completed ===",
             "total_entries", g_fts_scalar_cache.size());
  } else {
    LOG_WARN("=== FTS Scalar Cache initialization failed ===", K(ret));
  }
  
  return ret;
}

// TODO: 未来需要实现的函数
// 从 fts_doc_rowkey 表和主表自动填充
int build_fts_scalar_cache_from_tables(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  
  // 1. 查询 fts_doc_rowkey 表获取 doc_id → rowkey 映射
  //    SELECT doc_id, rowkey FROM fts_doc_rowkey WHERE ...
  
  // 2. 根据 rowkey 查询主表获取标量字段
  //    SELECT id, base_id, docid_col FROM items1 WHERE id = rowkey
  
  // 3. 填充 HashMap
  //    g_fts_scalar_cache.add_mapping(doc_id, id, base_id, docid_col)
  
  LOG_INFO("TODO: build_fts_scalar_cache_from_tables not implemented yet");
  ret = OB_NOT_IMPLEMENT;
  
  return ret;
}

} // namespace sql
} // namespace oceanbase
