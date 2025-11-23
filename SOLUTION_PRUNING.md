# 全文检索剪枝解决方案

## 方案概述

基于对代码的深入分析，我找到了问题的根源：**你修改的位置是正确的，但需要在 DAAT 算法层面添加额外的过滤逻辑**。

## 核心问题

当前你在 `gen_inv_idx_scan_default_range()` 和 `gen_inv_idx_scan_one_range()` 中硬编码 docid=1，但 DAAT 算法会持续调用 `dim_iter->get_next_row()` 来获取更多文档，直到满足 LIMIT 条件。即使单个 posting list 被限制了，其他 token 的 posting list 仍然可以提供足够的文档。

## 解决方案 A：在 DAAT 层添加 docid 过滤（推荐）

### 步骤 1：在 ObSRDaaTIterImpl 中添加 docid 上界字段

**文件**：`src/storage/retrieval/ob_sparse_daat_iter.h`

在 `ObSRDaaTIterImpl` 类中添加：

```cpp
class ObSRDaaTIterImpl : public ObISparseRetrievalMergeIter
{
  // ... existing members ...
protected:
  // 添加 docid 上界字段
  uint64_t docid_upper_bound_;  // 0 表示无限制
  bool has_docid_upper_bound_;
  
  // 添加检查方法
  bool should_skip_docid(const ObDatum &id_datum);
};
```

### 步骤 2：在 collect_dims_by_id 中过滤 docid

**文件**：`src/storage/retrieval/ob_sparse_daat_iter.cpp`

修改 `collect_dims_by_id()` 方法（行 347-398）：

```cpp
int ObSRDaaTIterImpl::collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;

  const ObSRMergeItem *top_item = nullptr;
  bool curr_doc_end = false;
  int64_t iter_idx = 0;
  relevance = 0.0;
  got_valid_id = false;

  while (OB_SUCC(ret) && !merge_heap_->empty() && !curr_doc_end) {
    if (merge_heap_->is_unique_champion()) {
      curr_doc_end = true;
    }
    if (OB_FAIL(merge_heap_->top(top_item))) {
      LOG_WARN("failed to get top item from merge heap", K(ret));
    } else if (OB_FAIL(relevance_collector_->collect_one_dim(top_item->iter_idx_, top_item->relevance_))) {
      LOG_WARN("failed to collect one dimension", K(ret));
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_FAIL(merge_heap_->pop())) {
      LOG_WARN("failed to pop top item in heap", K(ret));
    } else {
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
    }
  }

  if (OB_SUCC(ret)) {
    id_datum = iter_domain_ids_[iter_idx];
    LOG_DEBUG("collect one dim", KPC(id_datum));
    if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null id datum", K(ret));
    } 
    // ===== 添加 docid 过滤逻辑 =====
    else if (has_docid_upper_bound_ && should_skip_docid(*id_datum)) {
      // 跳过超出上界的 docid，继续处理下一个
      got_valid_id = false;
      relevance_collector_->reset_current();  // 重置当前收集的 relevance
      LOG_DEBUG("sr pruning: skip docid beyond upper bound", K(*id_datum), K_(docid_upper_bound));
    }
    // ===== 结束添加 =====
    else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
      LOG_WARN("failed to get result", K(ret));
    } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
      LOG_WARN("failed to process collected row", K(ret));
    }
  }

  // ... rest of the method ...
}
```

### 步骤 3：实现 should_skip_docid 方法

在 `ob_sparse_daat_iter.cpp` 中添加：

```cpp
bool ObSRDaaTIterImpl::should_skip_docid(const ObDatum &id_datum)
{
  bool should_skip = false;
  if (has_docid_upper_bound_) {
    // 假设 docid 是 uint64 类型
    if (iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type) {
      uint64_t docid = id_datum.get_uint64();
      should_skip = (docid > docid_upper_bound_);
    } else {
      // 对于其他类型，需要根据实际情况处理
      // 这里暂时不过滤
      should_skip = false;
    }
  }
  return should_skip;
}
```

### 步骤 4：从谓词中提取 docid 上界

这是最关键的一步：需要找到 `id < 1000` 这个谓词，并将其转换为 docid 上界。

**可能的位置**：

1. **在 ObDASTRMergeIter::init() 中提取**

   查看 `ir_ctdef_` 或 `ir_rtdef_` 中是否有存储主表谓词的字段。

2. **从 ObDASIRScanCtDef 中获取**

   检查是否有 `pushdown_filters_` 或类似字段。

3. **临时方案：硬编码上界**

   在 `ObTextDaaTIter::init()` 中：

   ```cpp
   int ObTextDaaTIter::init(const ObTextDaaTParam &param)
   {
     // ... existing code ...
     
     // 临时硬编码 docid 上界
     docid_upper_bound_ = 1000;  // 对应 id < 1000
     has_docid_upper_bound_ = true;
     LOG_INFO("sr pruning: docid upper bound set", K_(docid_upper_bound));
     
     return ret;
   }
   ```

## 解决方案 B：在 Token Iter 层过滤（更底层）

### 修改 ObTextRetrievalTokenIter::get_next_batch()

**文件**：`src/storage/retrieval/ob_text_retrieval_token_iter.cpp`

在读取倒排索引后，立即过滤超出上界的 docid：

```cpp
int ObTextRetrievalTokenIter::get_next_batch(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  // ... existing code to read from inv_idx_scan_iter_ ...
  
  // 假设已经读取了 count 行数据
  // 添加过滤逻辑
  const uint64_t DOCID_UPPER_BOUND = 1000;  // 临时硬编码
  int64_t filtered_count = 0;
  
  for (int64_t i = 0; i < count; ++i) {
    ObDatum &doc_id_datum = inv_scan_domain_id_col_->locate_expr_datum(*eval_ctx_, i);
    uint64_t docid = doc_id_datum.get_uint64();
    
    if (docid <= DOCID_UPPER_BOUND) {
      // 保留这个 docid
      if (i != filtered_count) {
        // 需要移动数据
        // 复制所有相关列的数据
      }
      ++filtered_count;
    } else {
      // 跳过这个 docid
      LOG_DEBUG("sr pruning: skip docid in token iter", K(docid), K(DOCID_UPPER_BOUND));
    }
  }
  
  count = filtered_count;
  
  // ... rest of the method ...
}
```

## 解决方案 C：完全禁用 Function Lookup（验证用）

### 临时禁用 set_children_iter_rangekey

**文件**：`src/sql/das/iter/sparse_retrieval/ob_das_tr_merge_iter.cpp`

```cpp
int ObDASTRMergeIter::set_children_iter_rangekey(
    const common::ObIArray<std::pair<ObDocIdExt, int>> &virtual_rangekeys, 
    const int64_t batch_size)
{
  // 临时禁用 function lookup，强制使用 default range
  LOG_INFO("sr debug: set_children_iter_rangekey disabled for testing");
  return OB_SUCCESS;
}
```

这样系统会使用你在 `gen_inv_idx_scan_default_range()` 中设置的 range，可以验证该 range 是否真的能限制倒排索引扫描。

## 推荐实施步骤

### 第一阶段：验证（1-2小时）

1. 实施**方案 C**，验证禁用 function lookup 后，你的 range 修改是否生效
2. 观察日志和性能指标，确认倒排索引扫描确实被限制了

### 第二阶段：实现过滤（2-3小时）

1. 实施**方案 A 的步骤 3**（临时硬编码版本）
2. 在 `ObTextDaaTIter::init()` 中设置 `docid_upper_bound_ = 1000`
3. 在 `ObSRDaaTIterImpl::collect_dims_by_id()` 中添加过滤逻辑
4. 测试并验证 recall 和 RT 的变化

### 第三阶段：完整实现（4-6小时）

1. 找到谓词下推信息的存储位置
2. 实现从 `id < 1000` 到 `docid_upper_bound_` 的自动提取
3. 处理各种边界情况（如 `id BETWEEN`、`id IN` 等）
4. 完整测试

## 关键注意事项

1. **docid 与主键 id 的映射**：
   - 需要确认 `domain_id` 确实等于主键 `id`
   - 如果不是直接相等，需要找到映射关系

2. **多个 token 的协调**：
   - DAAT 算法会从多个 token 的 posting list 中合并结果
   - 过滤需要在合并之后、返回结果之前进行

3. **性能考虑**：
   - 方案 A 在 DAAT 层过滤，开销较小
   - 方案 B 在 Token Iter 层过滤，可以更早剪枝，但实现复杂

4. **Function Lookup 模式**：
   - 这是一个优化模式，用于减少不必要的主表 lookup
   - 不应该完全禁用，而是要让它与剪枝逻辑兼容

## 调试建议

### 添加详细日志

在以下位置添加日志：

```cpp
// 1. 在 gen_inv_idx_scan_default_range() 中
LOG_INFO("SR PRUNING: default range", 
         K(query_token), 
         "start_docid", scan_range.start_key_.get_obj_ptr()[1].get_uint64(),
         "end_docid", scan_range.end_key_.get_obj_ptr()[1].get_uint64());

// 2. 在 fill_merge_heap() 中
LOG_INFO("SR PRUNING: heap item", 
         K(iter_idx), 
         K(item.relevance_),
         "docid", iter_domain_ids_[iter_idx]->get_uint64());

// 3. 在 collect_dims_by_id() 中
LOG_INFO("SR PRUNING: collected docid", 
         K(*id_datum), 
         K(relevance),
         K_(docid_upper_bound),
         "skipped", should_skip);
```

### 性能指标

监控以下指标来验证剪枝是否生效：

1. **倒排索引扫描的行数**：应该显著减少
2. **DAAT 处理的文档数**：应该减少到上界以内
3. **查询响应时间**：应该有明显下降
4. **Recall@10**：应该根据上界的设置而变化

## 总结

你的思路是正确的，问题在于：

1. **你修改的位置（gen_inv_idx_scan_default_range）是对的**，但在 function lookup 模式下会被覆盖
2. **真正需要剪枝的位置是 DAAT 算法层**，在 `collect_dims_by_id()` 或 `fill_merge_heap()` 中
3. **最简单的验证方法**：临时禁用 `set_children_iter_rangekey()`，看你的 range 修改是否生效
4. **最终解决方案**：在 DAAT 层添加 docid 过滤逻辑，并从谓词中提取上界

建议先实施方案 C 进行验证，然后实施方案 A 的临时硬编码版本，最后再实现完整的谓词提取逻辑。

