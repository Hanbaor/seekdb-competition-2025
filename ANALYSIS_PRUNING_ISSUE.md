# 全文检索混合查询剪枝问题分析

## 问题描述

在执行以下 SQL 时，希望通过标量谓词 `id < 1000` 来剪枝倒排索引的扫描范围：

```sql
SELECT `docid_col`, MATCH(`fulltext_col`) AGAINST(`text`) as _score
FROM `items1`
WHERE MATCH(fulltext_col) AGAINST(`text`)
  and base_id in ('base_id_1', 'base_id_2', 'base_id_3')
  and id < 1000
ORDER BY _score DESC
LIMIT 10
```

**当前问题**：即使在 `gen_inv_idx_scan_default_range()` 和 `gen_inv_idx_scan_one_range()` 中硬编码 docid=1，BM25 结果和性能依旧没有变化，说明这些 range 被后续流程覆盖或忽略了。

## 架构分析

### 1. 执行链路概览

```
SQL 解析 (ObDMLResolver)
    ↓
优化器 (ObLogPlan)
    ↓
DAS 层 (ObDASTRMergeIter)
    ↓
    ├─ 初始化扫描参数 (init_das_iter_scan_params)
    │   └─ 生成倒排索引扫描范围 (gen_inv_idx_scan_default_range)
    │       └─ 设置 inv_scan_params_[i]->key_ranges_
    ↓
    ├─ 创建维度迭代器 (create_dim_iters)
    │   └─ ObTextRetrievalTokenIter (每个 token 一个)
    ↓
    ├─ 创建稀疏检索迭代器 (create_sparse_retrieval_iter)
    │   └─ ObSRLookupIter (function lookup 模式)
    │       └─ ObTextDaaTIter (DAAT 算法)
    ↓
Storage 层
    ├─ ObDASScanIter (包装 storage 迭代器)
    │   └─ 使用 inv_idx_scan_param_ (ObTableScanParam)
    │       └─ key_ranges_ (扫描范围数组)
    ↓
    └─ 实际的倒排索引扫描
        └─ 读取 posting list
```

### 2. 倒排索引表结构

倒排索引的 rowkey 结构（`INV_IDX_ROWKEY_COL_CNT = 2`）：
- **第1列**: `token` (varchar) - 分词后的词元
- **第2列**: `domain_id` (uint64) - **这就是 docid**

**关键发现**：`domain_id` 在倒排索引中就是文档的唯一标识符（docid），它对应主表的某个唯一键（可能是主键 `id`，也可能是内部生成的 rowid）。

### 3. 问题根源分析

#### 3.1 当前的剪枝尝试位置

在 `ob_das_tr_merge_iter.cpp` 中：

```cpp
// 行 803-848: gen_inv_idx_scan_default_range
int ObDASTRMergeIter::gen_inv_idx_scan_default_range(const ObString &query_token, ObNewRange &scan_range)
{
  // ...
  obj_ptr[1].set_min_value();  // 原本是 min
  obj_ptr[3].set_max_value();  // 原本是 max
  
  // 你的修改：
  const uint64_t DEBUG_DOCID_UPPER_BOUND = 1;
  start_objs[1].set_uint64(0);
  end_objs[1].set_uint64(DEBUG_DOCID_UPPER_BOUND);  // 硬编码为 1
  
  // 这个 range 被 push 到:
  inv_scan_params_[i]->key_ranges_.push_back(inv_idx_scan_range);
}
```

#### 3.2 为什么剪枝没生效？

**核心问题**：在 **function lookup 模式**下，`set_children_iter_rangekey()` 会被调用，它会：

1. **覆盖之前设置的 key_ranges_**（行 726-801）
2. 使用 `virtual_rangekeys` 参数中的 docid 批次来生成新的 range
3. 这些 `virtual_rangekeys` 来自于 **上层的 DAAT/TAAT 算法**，包含了所有候选文档的 docid

```cpp
// 行 726-801: set_children_iter_rangekey
int ObDASTRMergeIter::set_children_iter_rangekey(
    const common::ObIArray<std::pair<ObDocIdExt, int>> &virtual_rangekeys, 
    const int64_t batch_size)
{
  // ...
  for (int64_t i = 0; i < dim_iter_cnt; ++i) {
    for (int64_t j = 0; j < batch_size; ++j) {
      // 使用 virtual_rangekeys 中的 docid 生成 range
      gen_inv_idx_scan_one_range(query_tokens_[i],
                                 virtual_rangekeys.at(j).first,  // ← 这里的 docid 来自上层
                                 inv_idx_scan_range);
      
      // 你的硬编码在这里也设置了 docid=1
      end_objs[1].set_uint64(DEBUG_DOCID_UPPER_BOUND);
      
      // 但问题是：这个 range 是针对特定 docid 的点查询！
      inv_scan_params_[i]->key_ranges_.push_back(inv_idx_scan_range);
    }
  }
}
```

**关键矛盾**：
- `gen_inv_idx_scan_default_range()` 生成的是 **全范围扫描** `[token, min_docid] ~ [token, max_docid]`
- `gen_inv_idx_scan_one_range()` 生成的是 **点查询** `[token, specific_docid]`
- 在 function lookup 模式下，系统使用的是 **点查询模式**，所以你修改的 default range 根本不会被使用！

### 4. docid 与主键 id 的映射关系

#### 4.1 docid 的来源

从代码追踪：

```cpp
// ob_text_retrieval_token_iter.cpp:200-209
int ObTextRetrievalTokenIter::get_inv_idx_scan_doc_id(ObDocIdExt &doc_id)
{
  ObDatum &doc_id_datum = inv_scan_domain_id_col_->locate_expr_datum(*eval_ctx_);
  doc_id.from_datum(doc_id_datum);
}
```

`inv_scan_domain_id_col_` 是从倒排索引扫描结果中读取的 **第2列** (domain_id)。

#### 4.2 domain_id 是什么？

根据代码和命名推断：
- `domain_id` 在全文检索中就是 **文档ID**（docid）
- 它应该对应主表的某个唯一标识，可能是：
  1. **主键 `id`** 的值
  2. **内部 rowid** (如果主表没有显式主键)
  3. **复合主键的编码**

**验证方法**：查看倒排索引表的实际数据，看 `domain_id` 列的值是否与主表的 `id` 列一致。

### 5. 正确的剪枝位置

#### 5.1 问题诊断

当前的剪枝失败有两个可能原因：

**原因1：Function Lookup 模式的执行流程**
```
1. 初始化时调用 gen_inv_idx_scan_default_range() 
   → 设置 key_ranges_ (但这个会被覆盖)

2. 执行时调用 set_children_iter_rangekey()
   → **清空并重新设置 key_ranges_**
   → 使用 virtual_rangekeys 中的 docid 批次

3. virtual_rangekeys 来自哪里？
   → ObSRLookupIter::get_next_batch()
   → ObTextDaaTIter 的 DAAT 算法
   → 从所有 token 的 posting list 中取交集/并集
```

**原因2：docid 的生成时机太晚**

当前流程：
```
扫描倒排索引 → 得到 docid → 用 docid 去主表 lookup → 应用 id < 1000 过滤
```

理想流程：
```
解析 id < 1000 → 转换为 docid 上界 → 限制倒排索引扫描范围
```

#### 5.2 真正的剪枝入口

需要在 **生成 virtual_rangekeys 之前** 就限制 docid 范围。有以下几个可能的切入点：

**切入点1：ObTextRetrievalTokenIter::get_next_batch()**
- 位置：`ob_text_retrieval_token_iter.cpp`
- 这是实际读取倒排索引的地方
- 可以在这里添加 docid 上界过滤

**切入点2：ObDASScanIter::do_table_scan()**
- 位置：`ob_das_scan_iter.cpp`
- Storage 层的扫描入口
- 可以在这里检查并应用 key_ranges_ 的限制

**切入点3：ObTableScanParam 的初始化**
- 在 `init_das_iter_scan_params()` 中
- 需要从主表的谓词 `id < 1000` 推导出 docid 上界
- 然后设置到 `inv_scan_params_[i]->key_ranges_`

### 6. 解决方案

#### 方案A：禁用 Function Lookup 模式（临时方案）

修改 `ob_das_tr_merge_iter.cpp`，强制使用 default range：

```cpp
int ObDASTRMergeIter::set_children_iter_rangekey(...)
{
  // 临时禁用，让系统使用 init_das_iter_scan_params() 中设置的 range
  return OB_SUCCESS;
}
```

**优点**：可以快速验证 default range 是否生效
**缺点**：会破坏 function lookup 的优化，性能可能下降

#### 方案B：在 virtual_rangekeys 生成时过滤（推荐）

修改 `ObTextDaaTIter` 或 `ObSRLookupIter`，在生成 `virtual_rangekeys` 时就应用 docid 上界：

```cpp
// 在 ob_sparse_lookup_iter.cpp 中
int ObSRLookupIter::get_next_batch(...)
{
  // 获取 docid 上界（从谓词下推信息中）
  uint64_t docid_upper_bound = get_docid_upper_bound_from_predicate();
  
  // 在收集 virtual_rangekeys 时过滤
  for (...) {
    ObDocIdExt doc_id = ...;
    if (doc_id.get_datum().get_uint64() <= docid_upper_bound) {
      virtual_rangekeys.push_back(doc_id);
    }
  }
}
```

#### 方案C：在倒排索引扫描层添加 range 过滤（最彻底）

修改 Storage 层的扫描逻辑，确保 `key_ranges_` 中的 docid 上界被正确应用：

1. 在 `ObTableScanParam` 中添加 `docid_upper_bound_` 字段
2. 在 `ObDASTRMergeIter::init_das_iter_scan_params()` 中设置这个字段
3. 在 Storage 层扫描时检查这个字段

### 7. 下一步行动

#### 7.1 验证 docid 与主键的映射关系

```sql
-- 查看倒排索引表的结构和数据
SELECT * FROM __all_virtual_table WHERE table_name LIKE '%inv_idx%';

-- 查看实际的 docid 值
SELECT token, domain_id FROM items1_inv_idx LIMIT 10;

-- 对比主表的 id
SELECT id FROM items1 LIMIT 10;
```

#### 7.2 添加详细日志

在以下位置添加日志：

```cpp
// 1. 在 gen_inv_idx_scan_default_range() 中
LOG_INFO("SR DEBUG: default range created", 
         K(query_token), 
         K(scan_range.start_key_), 
         K(scan_range.end_key_));

// 2. 在 set_children_iter_rangekey() 调用后
LOG_INFO("SR DEBUG: key_ranges after set_children_iter_rangekey", 
         K(inv_scan_params_[i]->key_ranges_.count()),
         K(inv_scan_params_[i]->key_ranges_));

// 3. 在 ObDASScanIter::do_table_scan() 中
LOG_INFO("SR DEBUG: actual scan ranges used", 
         K(scan_param_->key_ranges_));

// 4. 在 ObTextRetrievalTokenIter::get_next_batch() 中
LOG_INFO("SR DEBUG: docid from posting", K(doc_id));
```

#### 7.3 找到 docid 上界的传递路径

需要找到从 SQL 谓词 `id < 1000` 到 docid 上界的完整路径：

```
SQL: id < 1000
  ↓ (Optimizer)
ObLogPlan → 生成 filter 表达式
  ↓ (Code Generation)
ObDASIRScanCtDef → 存储谓词信息
  ↓ (Runtime)
ObDASTRMergeIter → 需要在这里提取上界
  ↓
inv_scan_params_[i]->key_ranges_ → 设置 docid 范围
```

关键是找到 `id < 1000` 在哪个阶段被转换为可用的信息。

## 总结

**问题根源**：
1. Function lookup 模式下，`set_children_iter_rangekey()` 会覆盖初始化时设置的 key_ranges_
2. 硬编码 docid=1 确实被设置了，但因为是点查询模式，系统仍能从其他 token/batch 获取足够的文档
3. 缺少从主键谓词 `id < 1000` 到 docid 上界的映射和传递机制
4. **核心问题**：DAAT 算法在 `fill_merge_heap()` 中从各个 token 的 posting list 中读取 docid，这个过程完全独立于你设置的 range

**真正的执行流程**：
```
ObSRDaaTIterImpl::get_next_rows()
  ↓
do_one_merge_round()
  ↓
fill_merge_heap()  ← 这里从每个 token iter 读取下一个 docid
  ↓
  for each token:
    dim_iter->get_next_row()  ← 调用 ObTextRetrievalDaaTTokenIter::get_next_row()
      ↓
      token_iter_->get_next_batch()  ← 调用 ObTextRetrievalTokenIter::get_next_batch()
        ↓
        inv_idx_scan_iter_->get_next_rows()  ← 这里才真正读取倒排索引
          ↓
          Storage 层使用 inv_idx_scan_param_->key_ranges_
```

**为什么你的修改没生效**：
1. 在 non-function-lookup 模式下，`gen_inv_idx_scan_default_range()` 生成的 range 会被使用
2. 但在 function-lookup 模式下，`set_children_iter_rangekey()` 会被调用，生成针对特定 docid 的点查询
3. 即使你把 docid 限制为 1，DAAT 算法仍会继续调用 `get_next_row()` 从其他 posting 中获取更多文档
4. 你需要在 **DAAT 的 fill_merge_heap() 或 collect_dims_by_id() 中** 过滤掉超出上界的 docid

**解决方向**：
1. 首先验证 `domain_id` 与主键 `id` 的映射关系
2. 找到谓词下推信息的存储位置（可能在 `ObDASIRScanCtDef` 或 `ObDASIRScanRtDef` 中）
3. **最佳位置**：在 `ObSRDaaTIterImpl::collect_dims_by_id()` 中添加 docid 过滤逻辑
4. 或者在 `ObTextRetrievalTokenIter::get_next_batch()` 中跳过超出上界的 docid

**关键代码位置**：
- DAS 层：`src/sql/das/iter/sparse_retrieval/ob_das_tr_merge_iter.cpp`
- Token 迭代器：`src/storage/retrieval/ob_text_retrieval_token_iter.cpp`
- Lookup 迭代器：`src/storage/retrieval/ob_sparse_lookup_iter.cpp`
- **DAAT 算法（最关键）**：`src/storage/retrieval/ob_sparse_daat_iter.cpp` 
  - `fill_merge_heap()` (行 309-345)
  - `collect_dims_by_id()` (行 347-398)
  - 在这里添加 docid 上界检查！

