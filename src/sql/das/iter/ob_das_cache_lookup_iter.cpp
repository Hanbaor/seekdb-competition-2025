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
#include "sql/das/iter/ob_das_cache_lookup_iter.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/das/ob_das_ir_define.h"
#include "sql/das/ob_fts_scalar_cache.h"
#include "sql/das/ob_fts_bm25s_index.h"
#include "sql/das/iter/sparse_retrieval/ob_das_tr_merge_iter.h"
#include "storage/concurrency_control/ob_data_validation_service.h"
// 新系统：通用 FTS 缓存
#include "sql/das/ob_fts_generic_cache.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

int ObDASCacheLookupIter::IndexProjRowStore::init(common::ObIAllocator &allocator,
                                                  const common::ObIArray<ObExpr*> &exprs,
                                                  ObEvalCtx *eval_ctx,
                                                  int64_t max_size)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(eval_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected nullptr for init index proj row store", K(ret));
  } else if (OB_ISNULL(store_rows_ =
      static_cast<ObChunkDatumStore::LastStoredRow*>(allocator.alloc(max_size * sizeof(ObChunkDatumStore::LastStoredRow))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory", K(max_size), K(ret));
  } else if (FALSE_IT(index_scan_proj_exprs_.set_allocator(&allocator))) {
  } else if (OB_FAIL(index_scan_proj_exprs_.assign(exprs))) {
    LOG_WARN("failed to assign index scan proj exprs", K(max_size), K(ret));
  } else {
    eval_ctx_ = eval_ctx;
    max_size_ = max_size;
    for (int64_t i = 0; i < max_size_; i++) {
      new (store_rows_ + i) ObChunkDatumStore::LastStoredRow(allocator);
      store_rows_[i].reuse_ = true;
    }
  }

  return ret;
}

int ObDASCacheLookupIter::IndexProjRowStore::save(bool is_vectorized, int64_t size)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(index_scan_proj_exprs_.count() == 0)) {
    // do nothing
  } else if (OB_UNLIKELY(size + saved_size_ > max_size_) ||
             OB_ISNULL(store_rows_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error for save store rows", K(size), K(max_size_), K(store_rows_), K(ret));
  } else {
    if (is_vectorized) {
      ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
      batch_info_guard.set_batch_size(size);
      for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
        batch_info_guard.set_batch_idx(i);
        if (OB_FAIL(store_rows_[i + saved_size_].save_store_row(index_scan_proj_exprs_, *eval_ctx_))) {
          LOG_WARN("cache lookup iter failed to store rows", K(ret));
        }
      }
    } else if (OB_FAIL(store_rows_[saved_size_].save_store_row(index_scan_proj_exprs_, *eval_ctx_))) {
      LOG_WARN("cache lookup iter failed to store rows", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    cur_idx_ = 0;
    saved_size_ += size;
  }

  return ret;
}

int ObDASCacheLookupIter::IndexProjRowStore::to_expr(int64_t size)
{
  int ret = OB_SUCCESS;
  if ((OB_UNLIKELY(index_scan_proj_exprs_.count() == 0))) {
    // do nothing
  } else if (OB_UNLIKELY(cur_idx_ == OB_INVALID_INDEX) ||
             OB_UNLIKELY(size + cur_idx_ > saved_size_) ||
             OB_ISNULL(store_rows_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error for convert store rows", K(size), K(max_size_), K(store_rows_), K(ret));  
  } else {
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
    batch_info_guard.set_batch_size(size);
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      batch_info_guard.set_batch_idx(i);
      if (OB_FAIL(store_rows_[cur_idx_].store_row_->to_expr<true>(index_scan_proj_exprs_, *eval_ctx_))) {
        LOG_WARN("cache lookup iter failed to convert store row to expr", K(ret));
      } else {
        cur_idx_++;
      }
    }
  }
  return ret;
}

void ObDASCacheLookupIter::IndexProjRowStore::IndexProjRowStore::reuse()
{
  cur_idx_ = OB_INVALID_INDEX;
  saved_size_ = 0;
  iter_end_ = false;
}

void ObDASCacheLookupIter::IndexProjRowStore::IndexProjRowStore::reset()
{
  if (OB_NOT_NULL(store_rows_)) {
    for (int64_t i = 0; i < max_size_; i++) {
      store_rows_[i].ObChunkDatumStore::LastStoredRow::~LastStoredRow();
    }
    store_rows_ = nullptr;
  }
  index_scan_proj_exprs_.reset();
  eval_ctx_ = nullptr;
  max_size_ = 1;
  saved_size_ = 0;
  cur_idx_ = OB_INVALID_INDEX;
  iter_end_ = false;
}

int ObDASCacheLookupIter::inner_init(ObDASIterParam &param)
{
  int ret = OB_SUCCESS;
  ObDASCacheLookupIterParam &lookup_param = static_cast<ObDASCacheLookupIterParam&>(param);
  const int64_t default_batch_row_count = lookup_param.default_batch_row_count_;
  const int64_t simulate_batch_row_cnt = - EVENT_CALL(EventTable::EN_TABLE_LOOKUP_BATCH_ROW_COUNT);
  const bool use_simulate_batch_row_cnt = simulate_batch_row_cnt > 0 && simulate_batch_row_cnt < default_batch_row_count;

  lookup_param.default_batch_row_count_  = use_simulate_batch_row_cnt ? simulate_batch_row_cnt : default_batch_row_count;
  LOG_DEBUG("simulate lookup row batch count", K(simulate_batch_row_cnt), K(default_batch_row_count));
  const bool bm25s_fast_path = g_bm25s_index.is_ready();
  if (OB_FAIL(ObDASLocalLookupIter::inner_init(param))) {
    LOG_WARN("failed to init das lookup iter", K(ret));
  } else if (!bm25s_fast_path && lookup_param.index_scan_proj_exprs_.count() > 0 &&
             OB_FAIL(index_proj_rows_.init(store_allocator_,
                                           lookup_param.index_scan_proj_exprs_, 
                                           lookup_param.eval_ctx_,
                                           lookup_param.default_batch_row_count_))) {
    LOG_WARN("failed to init index proj rows", K(ret));
  } else if (!bm25s_fast_path) {
    cached_results_.set_allocator(&store_allocator_);
    if (OB_FAIL(cached_results_.init(lookup_param.default_batch_row_count_))) {
      LOG_WARN("failed to init cached results", K(ret));
    }
  }

  return ret;
}

int ObDASCacheLookupIter::inner_reuse()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObDASLocalLookupIter::inner_reuse())) {
    LOG_WARN("failed to reuse das lookup iter", K(ret));
  } else {
    index_proj_rows_.reuse();
    bm25s_fast_path_done_ = false;  // 重置快速路径标志
  }
  return ret;
}

int ObDASCacheLookupIter::inner_release()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObDASLocalLookupIter::inner_release())) {
    LOG_WARN("failed to release das lookup iter", K(ret));
  } else {
    index_proj_rows_.reset();
    cached_doc_ids_.reset();  // ObSEArray 使用 reset
    cached_results_.destroy();
    store_allocator_.reset();
  }
  return ret;
}

void ObDASCacheLookupIter::reset_lookup_state()
{
  ObDASLookupIter::reset_lookup_state();
  index_proj_rows_.reuse();
  cached_doc_ids_.reuse();
  cached_results_.reuse();
}

int ObDASCacheLookupIter::add_rowkeys_and_collect(int64_t count)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(eval_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected nullptr", K_(eval_ctx));
  } else {
    const bool use_cache = g_fts_generic_cache.is_cache_file_opened();
    
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
    batch_info_guard.set_batch_size(count);
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      batch_info_guard.set_batch_idx(i);
      
      // 在正确的 batch_idx 下收集 doc_id
      if (rowkey_exprs_.count() > 0) {
        ObDatum &doc_id_datum = rowkey_exprs_.at(0)->locate_expr_datum(*eval_ctx_);
        uint64_t doc_id = doc_id_datum.get_uint64();
        if (OB_FAIL(cached_doc_ids_.push_back(doc_id))) {
          LOG_WARN("failed to push back doc id", K(ret));
        }
      }
      
      if (OB_SUCC(ret) && !use_cache && OB_FAIL(ObDASLocalLookupIter::add_rowkey())) {
        LOG_WARN("failed to add rowkey", K(ret), K(i));
      }
    }
    
  }
  return ret;
}

int ObDASCacheLookupIter::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  bool got_next_row = false;
  const bool need_index_proj = index_proj_rows_.index_scan_proj_exprs_.count() > 0;
  OB_ASSERT(need_index_proj ? index_proj_rows_.max_size_ == default_batch_row_count_ : true);

  do {
    switch (state_) {
      case INDEX_SCAN: {
        reset_lookup_state();
        while (OB_SUCC(ret) && !index_end_ && lookup_rowkey_cnt_ < default_batch_row_count_) {
          index_table_iter_->clear_evaluated_flag();
          if (OB_FAIL(index_table_iter_->get_next_row())) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
              LOG_WARN("failed to get next row from index table", K(ret));
            } else {
              index_end_ = true;
              ret = OB_SUCCESS;
            }
          } else {
            // 收集 doc_id（单行模式）
            if (rowkey_exprs_.count() > 0) {
              ObDatum &doc_id_datum = rowkey_exprs_.at(0)->locate_expr_datum(*eval_ctx_);
              uint64_t doc_id = doc_id_datum.get_uint64();
              if (OB_FAIL(cached_doc_ids_.push_back(doc_id))) {
                LOG_WARN("failed to push back doc id", K(ret));
              }
            }
            
            if (OB_SUCC(ret) && OB_FAIL(ObDASLocalLookupIter::add_rowkey())) {
              LOG_WARN("failed to add row key", K(ret));
            } else if (need_index_proj && OB_FAIL(index_proj_rows_.save(false, 1))) {
              LOG_WARN("save index proj rows failed", K(ret));
            } else {
              ++lookup_rowkey_cnt_;
            }
          }
        }

        if (OB_SUCC(ret)) {
          if (OB_LIKELY(lookup_rowkey_cnt_ > 0)) {
            state_ = DO_LOOKUP;
          } else {
            state_ = FINISHED;
          }
        }
        break;
      }

      case DO_LOOKUP: {
        if (OB_FAIL(do_index_lookup())) {
          LOG_WARN("failed to do index lookup", K(ret));
        } else {
          state_ = OUTPUT_ROWS;
        }
        break;
      }

      case OUTPUT_ROWS: {
        data_table_iter_->clear_evaluated_flag();
        if (OB_FAIL(data_table_iter_->get_next_row())) {
          if (OB_LIKELY(OB_ITER_END == ret)) {
            ret = OB_SUCCESS;
            if (OB_FAIL(check_index_lookup())) {
              LOG_WARN("failed to check table lookup", K(ret));
            } else {
              state_ = INDEX_SCAN;
            }
          } else {
            LOG_WARN("failed to get next row from data table", K(ret));
          }
        } else {
          got_next_row = true;
          ++lookup_row_cnt_;
          if (got_next_row && index_proj_rows_.have_data() &&
              OB_FAIL(index_proj_rows_.to_expr(1))) {
            LOG_WARN("failed to convert store row to expr", K(ret));
          }
        }
        break;
      }

      case FINISHED: {
        ret = OB_ITER_END;
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected lookup state", K_(state));
      }
    }
  } while (!got_next_row && OB_SUCC(ret));

  return ret;
}

int ObDASCacheLookupIter::inner_get_next_rows(int64_t &count, int64_t capacity)
{
  int ret = OB_SUCCESS;
  bool get_next_rows = false;
  const bool need_index_proj = index_proj_rows_.index_scan_proj_exprs_.count() > 0;
  
  do {
    switch (state_) {
      case INDEX_SCAN: {
        reset_lookup_state();
        
        if (OB_FAIL(ret) || index_end_) {
          // do nothing
        } else if (OB_FAIL(index_table_iter_->get_next_rows(count, capacity))) {
          if (OB_LIKELY(OB_ITER_END == ret)) {
            ret = OB_SUCCESS;
            index_end_ = true;
          } else {
            LOG_WARN("failed to get next rows from index iter", K(ret));
          }
        }
        
        if (OB_SUCC(ret) && count > 0) {
          if (OB_FAIL(add_rowkeys_and_collect(count))) {
            LOG_WARN("failed to add row keys and collect", K(ret));
          } else if (need_index_proj && OB_FAIL(index_proj_rows_.save(true, count))) {
            LOG_WARN("save index proj rows failed", K(ret));
          } else {
            lookup_rowkey_cnt_ += count;
          }
        }

        // 收集完毕后，统一检查缓存命中
        // ========== V3: 使用 KVCache 模式 ==========
        // 注意：缓存已在 create_text_retrieval_tree 入口处统一加载
        bool will_cache_hit = false;
        const bool use_cache = g_fts_generic_cache.is_cache_file_opened();
        
        if (OB_SUCC(ret) && lookup_rowkey_cnt_ > 0 && use_cache) {
          will_cache_hit = true;
          int64_t file_row_count = g_fts_generic_cache.get_file_header().row_count;
          for (int64_t i = 0; i < cached_doc_ids_.count(); ++i) {
            uint64_t doc_id = cached_doc_ids_.at(i);
            if (doc_id == 0 || doc_id > static_cast<uint64_t>(file_row_count)) {
              will_cache_hit = false;
              break;
            }
          }
          cache_verified_ = will_cache_hit;
        }

        if (OB_SUCC(ret)) {
          if (OB_LIKELY(lookup_rowkey_cnt_ > 0)) {
            if (will_cache_hit) {
              state_ = OUTPUT_ROWS;
            } else {
              state_ = DO_LOOKUP;
            }
          } else {
            state_ = FINISHED;
          }
        }
        break;
      }

      case DO_LOOKUP: {
        if (OB_FAIL(do_index_lookup())) {
          LOG_WARN("failed to do index lookup", K(ret));
        } else {
          state_ = OUTPUT_ROWS;
        }
        break;
      }

      case OUTPUT_ROWS: {
        count = 0;
        data_table_iter_->clear_evaluated_flag();
        
        const ObDASScanCtDef *lookup_ctdef = static_cast<const ObDASScanCtDef*>(lookup_ctdef_);
        int64_t total_doc_ids = cached_doc_ids_.count();
        
        // 只使用新系统 KVCache 模式（磁盘持久化）
        const bool cache_available = g_fts_generic_cache.is_cache_file_opened();
        const int64_t cache_size = cache_available ? g_fts_generic_cache.get_file_header().row_count : 0;
        const int64_t output_col_count = OB_NOT_NULL(lookup_ctdef) ? lookup_ctdef->result_output_.count() : 0;
        const ObDASIterType iter_type = OB_NOT_NULL(data_table_iter_) ? data_table_iter_->get_type() : DAS_ITER_INVALID;
        const bool can_try_cache = cache_available && total_doc_ids > 0 && OB_NOT_NULL(lookup_ctdef);
        const bool scan_cache_candidate = can_try_cache && iter_type == DAS_ITER_SCAN;
        
        bool all_cache_hit = false;
        int64_t cache_fill_count = 0;
        
        if (scan_cache_candidate) {
          cache_fill_count = std::min(total_doc_ids, capacity);
          ObDatum **col_datums = nullptr;
          int64_t *col_types = nullptr;
          if (output_col_count > 0) {
            col_datums = static_cast<ObDatum**>(alloca(sizeof(ObDatum*) * output_col_count));
            col_types = static_cast<int64_t*>(alloca(sizeof(int64_t) * output_col_count));
            
            for (int64_t j = 0; j < output_col_count; ++j) {
              ObExpr *expr = lookup_ctdef->result_output_.at(j);
              if (OB_NOT_NULL(expr)) {
                col_datums[j] = expr->locate_batch_datums(*eval_ctx_);
                col_types[j] = static_cast<int64_t>(expr->datum_meta_.type_);
              } else {
                col_datums[j] = nullptr;
                col_types[j] = 0;
              }
            }
          }
          
          // 一次遍历：填充数据（如果 cache_verified_ 为 true，跳过检查）
          all_cache_hit = true;
          
          // KVCache 模式：从文件/缓存加载行数据
          {
            int64_t file_row_count = cache_size;
            
            for (int64_t i = 0; OB_SUCC(ret) && i < cache_fill_count; ++i) {
              uint64_t doc_id = cached_doc_ids_.at(i);
              
              // 检查 doc_id 是否有效
              if (!cache_verified_ && (doc_id == 0 || doc_id > static_cast<uint64_t>(file_row_count))) {
                all_cache_hit = false;
                break;
              }
              
              // 从 LRU ObHashMap 获取行数据（LRU 淘汰策略，无 Hazard Pointer 开销）
              const ObFtsRowData *row = nullptr;
              int cache_ret = g_fts_generic_cache.get_row_from_lru(doc_id, row);
              if (OB_SUCCESS != cache_ret || OB_ISNULL(row)) {
                // 缓存未命中，走正常回表流程
                all_cache_hit = false;
                break;
              }
              
              int64_t int_col_order = 0;
              int64_t str_col_order = 0;
              
              for (int64_t j = 0; OB_SUCC(ret) && j < output_col_count; ++j) {
                if (OB_ISNULL(col_datums[j])) continue;
                
                ObDatum &datum = col_datums[j][i];
                ObObjType col_type = static_cast<ObObjType>(col_types[j]);
                
                if (ob_is_integer_type(col_type)) {
                  // 使用 find_column_by_type 获取正确的列索引
                  int64_t cache_col_idx = g_fts_generic_cache.find_column_by_type(ObIntType, int_col_order);
                  if (cache_col_idx >= 0) {
                    int64_t val = row->get_int_column_fast(cache_col_idx);
                    datum.set_uint(static_cast<uint64_t>(val));
                  } else {
                    datum.set_uint(0);
                  }
                  int_col_order++;
                } else if (ob_is_string_type(col_type)) {
                  // 使用 find_column_by_type 获取正确的列索引
                  int64_t cache_col_idx = g_fts_generic_cache.find_column_by_type(ObVarcharType, str_col_order);
                  if (cache_col_idx >= 0) {
                    ObString src = row->get_string_column_fast(cache_col_idx);
                    if (src.length() > 0 && OB_NOT_NULL(src.ptr())) {
                      char *buf = static_cast<char*>(eval_ctx_->exec_ctx_.get_allocator().alloc(src.length()));
                      if (OB_ISNULL(buf)) {
                        ret = OB_ALLOCATE_MEMORY_FAILED;
                        LOG_WARN("failed to allocate string memory", K(ret), K(src.length()));
                      } else {
                        MEMCPY(buf, src.ptr(), src.length());
                        datum.set_string(buf, src.length());
                      }
                    } else {
                      datum.set_string(nullptr, 0);
                    }
                  } else {
                    datum.set_string(nullptr, 0);
                  }
                  str_col_order++;
                }
              }
            }
          }
          
          // 重置标志
          cache_verified_ = false;
          
          // 如果全部命中缓存，设置表达式标志
          if (OB_SUCC(ret) && all_cache_hit) {
            for (int64_t j = 0; j < output_col_count; ++j) {
              ObExpr *expr = lookup_ctdef->result_output_.at(j);
              if (OB_NOT_NULL(expr)) {
                expr->set_evaluated_projected(*eval_ctx_);
              }
            }
            
            if (need_index_proj && index_proj_rows_.have_data()) {
              if (OB_FAIL(index_proj_rows_.to_expr(cache_fill_count))) {
                LOG_WARN("failed to convert store row to expr after cache hit", K(ret));
              }
            }
            if (OB_SUCC(ret)) {
              count = cache_fill_count;
              get_next_rows = true;
              lookup_row_cnt_ += count;
            }
          }
        }
        
        // === 正常回表流程 ===
        if (!all_cache_hit && OB_SUCC(ret) && OB_NOT_NULL(data_table_iter_)) {
          if (OB_FAIL(data_table_iter_->get_next_rows(count, capacity))) {
            if (OB_LIKELY(OB_ITER_END == ret)) {
              ret = OB_SUCCESS;
              if (count > 0) {
                lookup_row_cnt_ += count;
                get_next_rows = true;
              } else {
                if (OB_FAIL(check_index_lookup())) {
                  LOG_WARN("failed to check table lookup", K(ret));
                } else {
                  state_ = INDEX_SCAN;
                }
              }
            } else {
              LOG_WARN("failed to get next rows from data table", K(ret));
            }
          } else {
            lookup_row_cnt_ += count;
            get_next_rows = true;
          }
        }
        
        // 清理 doc_ids 缓存，准备下一批次
        cached_doc_ids_.reuse();
        
        // === 状态转换 ===
        if (OB_SUCC(ret) && all_cache_hit && get_next_rows) {
          if (OB_FAIL(check_index_lookup())) {
            LOG_WARN("failed to check table lookup after cache hit", K(ret));
          } else {
            state_ = INDEX_SCAN;
          }
        }
        
        // 处理索引投影列（只有正常回表时需要）
        if (OB_FAIL(ret)) {
        } else if (!all_cache_hit && get_next_rows && need_index_proj && index_proj_rows_.have_data() &&
                   OB_FAIL(index_proj_rows_.to_expr(count))) {
          LOG_WARN("failed to convert store row to expr", K(ret));
        }
        break;
      }

      case FINISHED: {
        ret = OB_ITER_END;
        break;
      }
    }
  } while (!get_next_rows && OB_SUCC(ret));

  return ret;
}

}  // namespace sql
}  // namespace oceanbase
