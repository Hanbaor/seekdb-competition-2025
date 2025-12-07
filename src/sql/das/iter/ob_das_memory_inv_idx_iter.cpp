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

#define USING_LOG_PREFIX SQL_DAS

#include "ob_das_memory_inv_idx_iter.h"
#include "lib/ob_define.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

ObDASMemoryInvIdxIter::ObDASMemoryInvIdxIter()
  : ObDASScanIter(ObDASIterType::DAS_ITER_SCAN),
    token_(),
    doc_ids_(nullptr),
    current_idx_(0),
    eval_ctx_(nullptr),
    domain_id_expr_(nullptr),
    is_inited_(false)
{
}

ObDASMemoryInvIdxIter::~ObDASMemoryInvIdxIter()
{
}

int ObDASMemoryInvIdxIter::init(const ObString &token,
                                const ObSEArray<int64_t, 16> *doc_ids,
                                ObEvalCtx *eval_ctx,
                                ObExpr *domain_id_expr)
{
  int ret = OB_SUCCESS;
  
  if (OB_ISNULL(doc_ids) || OB_ISNULL(eval_ctx) || OB_ISNULL(domain_id_expr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(doc_ids), KP(eval_ctx), KP(domain_id_expr));
  } else {
    token_ = token;
    doc_ids_ = doc_ids;
    eval_ctx_ = eval_ctx;
    domain_id_expr_ = domain_id_expr;
    current_idx_ = 0;
    is_inited_ = true;
    
    //LOG_INFO("[MEMORY_INV_IDX] initialized memory inverted index iterator",
    //         K(token), "doc_count", doc_ids_->count());
  }
  
  return ret;
}

int ObDASMemoryInvIdxIter::do_table_scan()
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("memory inverted index iterator not initialized", K(ret));
  } else {
    // 重置迭代位置
    current_idx_ = 0;
    //LOG_INFO("[MEMORY_INV_IDX] start table scan from memory cache",
    //         K(token_), "doc_count", doc_ids_->count());
  }
  
  return ret;
}

int ObDASMemoryInvIdxIter::rescan()
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("memory inverted index iterator not initialized", K(ret));
  } else {
    current_idx_ = 0;
    //LOG_INFO("[MEMORY_INV_IDX] rescan from memory cache", K(token_));
  }
  
  return ret;
}

int ObDASMemoryInvIdxIter::inner_reuse()
{
  current_idx_ = 0;
  return OB_SUCCESS;
}

int ObDASMemoryInvIdxIter::inner_release()
{
  is_inited_ = false;
  doc_ids_ = nullptr;
  eval_ctx_ = nullptr;
  domain_id_expr_ = nullptr;
  current_idx_ = 0;
  return OB_SUCCESS;
}

int ObDASMemoryInvIdxIter::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("memory inverted index iterator not initialized", K(ret));
  } else if (current_idx_ >= doc_ids_->count()) {
    ret = OB_ITER_END;
    // LOG_INFO("[MEMORY_INV_IDX] iteration ended", K(token_), K(current_idx_), "doc_count", doc_ids_->count());
  } else {
    // 从缓存中读取doc_id并填充到表达式中
    int64_t doc_id = doc_ids_->at(current_idx_);
    ObDatum &datum = domain_id_expr_->locate_datum_for_write(*eval_ctx_);
    datum.set_int(doc_id);
    domain_id_expr_->set_evaluated_projected(*eval_ctx_);
    
    current_idx_++;
    
    // LOG_INFO("[MEMORY_INV_IDX] get next row from memory",
    //          K(token_), K(doc_id), K(current_idx_), "doc_count", doc_ids_->count());
  }
  
  return ret;
}

int ObDASMemoryInvIdxIter::inner_get_next_rows(int64_t &count, int64_t capacity)
{
  int ret = OB_SUCCESS;
  
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("memory inverted index iterator not initialized", K(ret));
  } else {
    count = 0;
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
    batch_info_guard.set_batch_size(capacity);
    
    // 批量读取doc_id
    while (OB_SUCC(ret) && count < capacity && current_idx_ < doc_ids_->count()) {
      int64_t doc_id = doc_ids_->at(current_idx_);
      batch_info_guard.set_batch_idx(count);
      
      ObDatum &datum = domain_id_expr_->locate_datum_for_write(*eval_ctx_);
      datum.set_int(doc_id);
      
      current_idx_++;
      count++;
    }
    
    if (count > 0) {
      domain_id_expr_->set_evaluated_projected(*eval_ctx_);
      // LOG_INFO("[MEMORY_INV_IDX] get next batch from memory",
      //          K(token_), K(count), K(current_idx_), "doc_count", doc_ids_->count());
    } else if (current_idx_ >= doc_ids_->count()) {
      ret = OB_ITER_END;
    }
  }
  
  return ret;
}

} // namespace sql
} // namespace oceanbase
