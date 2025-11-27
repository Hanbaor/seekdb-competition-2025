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
 #include "storage/concurrency_control/ob_data_validation_service.h"
 
 #include "sql/das/ob_fts_scalar_cache.h"
 
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
 
   if (OB_FAIL(ObDASLocalLookupIter::inner_init(param))) {
     LOG_WARN("failed to init das lookup iter", K(ret));
   } else if (lookup_param.index_scan_proj_exprs_.count() > 0 &&
              OB_FAIL(index_proj_rows_.init(store_allocator_,
                                            lookup_param.index_scan_proj_exprs_, 
                                            lookup_param.eval_ctx_,
                                            lookup_param.default_batch_row_count_))) {
     LOG_WARN("failed to init index proj rows", K(ret));
   } else {
     cached_doc_ids_.set_allocator(&store_allocator_);
     cached_results_.set_allocator(&store_allocator_);
     if (OB_FAIL(cached_doc_ids_.init(lookup_param.default_batch_row_count_))) {
       LOG_WARN("failed to init cached doc ids", K(ret));
     } else {
       LOG_INFO("FTS_CACHE_TRACE", "event", "cached_doc_ids_inited",
                "capacity", lookup_param.default_batch_row_count_);
     }
     if (OB_SUCC(ret)) {
       if (OB_FAIL(cached_results_.init(lookup_param.default_batch_row_count_))) {
         LOG_WARN("failed to init cached results", K(ret));
       } else {
         LOG_INFO("FTS_CACHE_TRACE", "event", "cached_results_inited",
                  "capacity", lookup_param.default_batch_row_count_);
       }
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
     cached_doc_ids_.clear();
     cached_results_.clear();
   }
   return ret;
 }
 
 void ObDASCacheLookupIter::reset_lookup_state()
 {
   ObDASLookupIter::reset_lookup_state();
   index_proj_rows_.reuse();
   int tmp_ret = cached_doc_ids_.reserve(default_batch_row_count_);
   if (OB_SUCCESS != tmp_ret) {
     LOG_WARN_RET(tmp_ret, "failed to reserve cached doc id array", K(tmp_ret), K_(default_batch_row_count));
   }
   tmp_ret = cached_results_.reserve(default_batch_row_count_);
   if (OB_SUCCESS != tmp_ret) {
     LOG_WARN_RET(tmp_ret, "failed to reserve cached results array", K(tmp_ret), K_(default_batch_row_count));
   }
   cached_doc_ids_.clear();
   cached_results_.clear();
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
           } else if (OB_FAIL(add_rowkey())) {
             LOG_WARN("failed to add row key", K(ret));
           } else if (need_index_proj && OB_FAIL(index_proj_rows_.save(false, 1))) { // if need to project, save it
               LOG_WARN("save index proj rows failed", K(ret));
           } else {
             ++lookup_rowkey_cnt_;
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
         int64_t storage_count = 0;
         int64_t index_capacity = 0;
         while (OB_SUCC(ret) && !index_end_ && lookup_rowkey_cnt_ < default_batch_row_count_) {
           storage_count = 0;
           index_capacity = std::min(capacity, std::min(max_size_, default_batch_row_count_ - lookup_rowkey_cnt_));
           index_table_iter_->clear_evaluated_flag();
           if (OB_FAIL(index_table_iter_->get_next_rows(storage_count, index_capacity))) {
             if (OB_UNLIKELY(OB_ITER_END != ret)) {
               LOG_WARN("failed to get next rows from index table", K(ret));
             } else {
               if (storage_count == 0) {
                 index_end_ = true;
               }
               ret = OB_SUCCESS;
             }
           }
           if (OB_SUCC(ret) && storage_count > 0) {
             if (OB_FAIL(add_rowkeys(storage_count))) {
               LOG_WARN("failed to add row keys", K(ret));
             } else if (need_index_proj && OB_FAIL(index_proj_rows_.save(true, storage_count))) { // if need to project, save it
               LOG_WARN("save index proj rows failed", K(ret));
             } else {
               // Save doc_ids for cache lookup
               ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
               batch_info_guard.set_batch_size(storage_count);
               for (int64_t i = 0; OB_SUCC(ret) && i < storage_count; ++i) {
                 batch_info_guard.set_batch_idx(i);
                 if (rowkey_exprs_.count() > 0) {
                   // Assume the first rowkey expr is doc_id
                   ObDatum &doc_id_datum = rowkey_exprs_.at(0)->locate_expr_datum(*eval_ctx_);
                   uint64_t doc_id = doc_id_datum.get_uint64();
                   LOG_DEBUG("cache lookup doc id push", K(doc_id),
                             "cached_count", cached_doc_ids_.count());
                   if (OB_FAIL(cached_doc_ids_.push_back(doc_id))) {
                     LOG_WARN("failed to push back doc id", K(ret),
                              "count_before_push", cached_doc_ids_.count());
                   } else {
                     LOG_INFO("FTS_CACHE_TRACE", "event", "doc_id_collected", K(doc_id), "batch_idx", i,
                              "cached_count", cached_doc_ids_.count());
                   }
                 }
               }
               lookup_rowkey_cnt_ += storage_count;
               LOG_INFO("FTS_CACHE_TRACE", "event", "index_scan_batch_done", "batch_size", storage_count,
                        K(lookup_rowkey_cnt_), "cached_doc_ids", cached_doc_ids_.count());
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
        count = 0;
        data_table_iter_->clear_evaluated_flag();
        
        const ObDASScanCtDef *lookup_ctdef = static_cast<const ObDASScanCtDef*>(lookup_ctdef_);
        int64_t total_doc_ids = cached_doc_ids_.count();
        const bool cache_available = g_fts_scalar_cache.is_inited() && g_fts_scalar_cache.size() > 0;
        
        // ========== 尝试从缓存获取数据，跳过回表 ==========
        bool all_cache_hit = false;
        int64_t cache_fill_count = 0;
        
        if (cache_available && total_doc_ids > 0 && OB_NOT_NULL(lookup_ctdef)) {
          // 检查是否所有 doc_id 都在缓存中
          all_cache_hit = true;
          for (int64_t i = 0; i < total_doc_ids && all_cache_hit; ++i) {
            uint64_t doc_id = cached_doc_ids_.at(i);
            ScalarFields tmp_fields;
            if (OB_SUCCESS != g_fts_scalar_cache.get_scalar_fields(doc_id, tmp_fields)) {
              all_cache_hit = false;
            }
          }
          
          LOG_INFO("FTS_CACHE_HIT", "event", "cache_check", 
                   K(all_cache_hit), K(total_doc_ids), K(capacity));
          
          // 如果全部命中缓存，直接从缓存填充数据
          if (all_cache_hit) {
            int64_t output_col_count = lookup_ctdef->result_output_.count();
            cache_fill_count = std::min(total_doc_ids, capacity);
            
            // 清除表达式的 evaluated flags
            for (int64_t j = 0; j < output_col_count; ++j) {
              ObExpr *expr = lookup_ctdef->result_output_.at(j);
              if (OB_NOT_NULL(expr)) {
                expr->clear_evaluated_flag(*eval_ctx_);
              }
            }
            
            // 使用 BatchInfoScopeGuard 设置 batch 信息
            ObEvalCtx::BatchInfoScopeGuard batch_info_guard(*eval_ctx_);
            batch_info_guard.set_batch_size(cache_fill_count);
            
            // 遍历每一行，从缓存填充数据
            for (int64_t i = 0; OB_SUCC(ret) && i < cache_fill_count; ++i) {
              batch_info_guard.set_batch_idx(i);
              
              uint64_t doc_id = cached_doc_ids_.at(i);
              ScalarFields fields;
              int cache_ret = g_fts_scalar_cache.get_scalar_fields(doc_id, fields);
              
              if (OB_SUCCESS != cache_ret) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("cache hit check passed but get failed", K(ret), K(doc_id));
                break;
              }
              
              // 遍历输出列，填充数据
              // 根据日志分析，回表只返回 2 列：
              //   col_idx=0: 整数列 (id)
              //   col_idx=1: 字符串列 (docid_col，值为 "doc-en-XXXX")
              for (int64_t j = 0; OB_SUCC(ret) && j < output_col_count; ++j) {
                ObExpr *expr = lookup_ctdef->result_output_.at(j);
                if (OB_ISNULL(expr)) continue;
                
                ObDatum &datum = expr->locate_datum_for_write(*eval_ctx_);
                const ObObjType col_type = expr->datum_meta_.type_;
                
                if (col_type == ObIntType || col_type == ObUInt64Type) {
                  // 整数列 → 填充 id（与回表返回的 col_idx=0 一致）
                  datum.set_uint(fields.id_);
                  expr->set_evaluated_projected(*eval_ctx_);
                } else if (ob_is_string_type(col_type)) {
                  // 字符串列 → 填充 docid_col（与回表返回的 col_idx=1 一致）
                  // 注意：回表只返回 docid_col，不返回 base_id
                  ObString src = fields.docid_col_;
                  
                  // 复制字符串到表达式内存
                  if (src.length() > 0) {
                    char *buf = expr->get_str_res_mem(*eval_ctx_, src.length());
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
                  
                  if (OB_SUCC(ret)) {
                    expr->set_evaluated_projected(*eval_ctx_);
                  }
                }
              }
            }
            // batch_info_guard 在这里析构，会恢复原始 batch 信息
            // 这是正常行为，和其他使用 BatchInfoScopeGuard 的代码一致
            
            if (OB_SUCC(ret)) {
              count = cache_fill_count;
              lookup_row_cnt_ += count;
              get_next_rows = true;
              
              LOG_INFO("FTS_CACHE_HIT", "event", "cache_fill_success",
                       K(cache_fill_count), K(lookup_row_cnt_));
            }
          }
        }
        
        // ========== 如果缓存未全部命中，回退到正常回表 ==========
        if (!all_cache_hit && OB_SUCC(ret)) {
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
        
        // ========== 状态转换 ==========
        if (OB_SUCC(ret) && all_cache_hit && get_next_rows) {
          // 缓存命中且数据填充成功后，这批数据已全部处理完
          // 必须转换到 INDEX_SCAN 获取下一批，否则下次进入 OUTPUT_ROWS 时
          // cached_doc_ids_ 已空，会走正常回表路径但没有数据
          if (OB_FAIL(check_index_lookup())) {
            LOG_WARN("failed to check table lookup after cache hit", K(ret));
          } else {
            state_ = INDEX_SCAN;
            LOG_INFO("FTS_CACHE_HIT", "event", "state_to_index_scan",
                     K(index_end_), K(lookup_rowkey_cnt_), K(lookup_row_cnt_));
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
 