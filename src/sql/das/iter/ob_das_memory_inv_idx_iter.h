/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SQL_DAS_ITER_OB_DAS_MEMORY_INV_IDX_ITER_H_
#define OCEANBASE_SQL_DAS_ITER_OB_DAS_MEMORY_INV_IDX_ITER_H_

#include "sql/das/iter/ob_das_scan_iter.h"
#include "sql/das/ob_fts_token_doc_cache.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief 内存倒排索引迭代器
 * 从 ObFTSTokenDocCache 中读取完整的倒排索引，而不是从磁盘扫描
 * 这个迭代器模拟 ObDASScanIter 的行为，但数据来源是内存缓存
 */
class ObDASMemoryInvIdxIter : public ObDASScanIter
{
public:
  ObDASMemoryInvIdxIter();
  virtual ~ObDASMemoryInvIdxIter();

  /**
   * @brief 初始化内存倒排索引迭代器
   * @param token 查询的token
   * @param doc_ids 从缓存中获取的doc_id列表
   * @param eval_ctx 表达式求值上下文
   * @param domain_id_expr doc_id输出表达式
   */
  int init(const common::ObString &token,
           const common::ObSEArray<int64_t, 16> *doc_ids,
           ObEvalCtx *eval_ctx,
           ObExpr *domain_id_expr);

  virtual int do_table_scan() override;
  virtual int rescan() override;

protected:
  virtual int inner_init(ObDASIterParam &param) override { return common::OB_SUCCESS; }
  virtual int inner_reuse() override;
  virtual int inner_release() override;
  virtual int inner_get_next_row() override;
  virtual int inner_get_next_rows(int64_t &count, int64_t capacity) override;

private:
  common::ObString token_;  // 当前查询的token
  const common::ObSEArray<int64_t, 16> *doc_ids_;  // doc_id列表（来自缓存）
  int64_t current_idx_;  // 当前迭代位置
  ObEvalCtx *eval_ctx_;  // 表达式求值上下文
  ObExpr *domain_id_expr_;  // doc_id输出表达式
  bool is_inited_;

  DISALLOW_COPY_AND_ASSIGN(ObDASMemoryInvIdxIter);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_ITER_OB_DAS_MEMORY_INV_IDX_ITER_H_
