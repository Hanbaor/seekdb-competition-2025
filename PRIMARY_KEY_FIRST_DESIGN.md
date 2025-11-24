# 主键优先扫描架构设计方案

## 一、当前架构分析

### 1.1 现有执行流程

```
查询: SELECT * FROM table WHERE MATCH(text) AND base_id IN (...) AND id < 1000

当前路径（倒排索引优先）:
┌─────────────────────────────────────────────────────────────┐
│ 1. 倒排索引扫描 (ObDASTRMergeIter)                          │
│    - Rowkey: (token, doc_id)                                │
│    - 扫描: 220 tokens × 所有 doc_id                         │
│    - 数据量: 178万行 (有 LAYER 1 end_key=1000 优化)        │
│    - 无 base_id 过滤                                        │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 归并 (DaaT/TaaT 模式)                                    │
│    - 计算相关度得分                                          │
│    - 输出候选 doc_id 列表                                    │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. Functional Lookup (主表回表)                            │
│    - 这里才应用 base_id 过滤 (LAYER 8)                      │
│    - LAYER 5 兜底过滤 id > 1000                            │
└─────────────────────────────────────────────────────────────┘
                          ↓
                        返回结果
```

**瓶颈**：
- 倒排索引扫描 358万行（无优化）→ 178万行（有优化）
- base_id 过滤在回表阶段，无法减少倒排索引扫描量
- block-level 过滤效果有限（数据混合在同一块中）

### 1.2 正向索引现状

```cpp
// 正向索引 rowkey: (doc_id, token)
// 当前用途：仅用于计算 token_count (do_token_cnt_agg)
// 位置：ObTextRetrievalTokenIter::gen_fwd_idx_scan_range

正向索引扫描范围生成：
fwd_range_objs_[0] = doc_id;        // start doc_id
fwd_range_objs_[1] = MIN;           // start token
fwd_range_objs_[2] = doc_id;        // end doc_id  
fwd_range_objs_[3] = MAX;           // end token

// 单个 doc_id 的所有 token: (doc_id, MIN) → (doc_id, MAX)
```

**问题**：正向索引能力未充分利用！
- 可以做范围扫描：`(doc_id_start, MIN) → (doc_id_end, MAX)`
- 可以获取 doc_id 的所有 token
- 但目前只在相关度计算时逐个查询

---

## 二、主键优先扫描设计

### 2.1 核心思想

```
新路径（主键优先）:
┌─────────────────────────────────────────────────────────────┐
│ 1. 检测条件：id < 常量 && 常量较小（如 < 10000）            │
│    触发主键优先路径                                          │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 主键范围扫描                                             │
│    - 直接扫描主表: id ∈ [1, 1000]                          │
│    - 扫描量: ~20 blocks                                     │
│    - 同时获取: id, base_id, fulltext_col                   │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. base_id 内存过滤（可选）                                 │
│    - 由于 id→base_id 映射固定，可直接过滤                  │
│    - base_id_1: id 1-500                                    │
│    - base_id_2: id 501-1000                                 │
│    - base_id_3: id 1001-1500 (跳过)                        │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 正向索引扫描 + Token 匹配                                │
│    - 对每个有效 doc_id，扫描正向索引                        │
│    - Range: (doc_id, MIN) → (doc_id, MAX)                  │
│    - 获取该文档的所有 token                                 │
│    - 内存匹配: tokens ∩ query_tokens                       │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. 计算相关度并排序                                          │
│    - 使用倒排索引的相关度算法（BM25）                       │
│    - 或简化为 token 命中数排序                              │
└─────────────────────────────────────────────────────────────┘
                          ↓
                        返回结果
```

### 2.2 性能对比

| 阶段 | 倒排索引路径 | 主键优先路径 | 改善 |
|-----|------------|------------|-----|
| 索引扫描 | 178万行 (3740 blocks) | 1000行 (20 blocks) | **99.4%↓** |
| 正向索引 | 不使用 | 1000次查询 (~2000行) | - |
| 回表 | base_id过滤后 | 无需单独回表 | - |
| **总扫描** | **178万行** | **~3000行** | **99.8%↓** |

---

## 三、实现方案

### 3.1 路径选择逻辑

在 `ObDASTRMergeIter::inner_init` 中添加条件检测：

```cpp
bool should_use_primary_key_path() {
  // 检测是否有 id < 常量 条件
  ObExpr *id_filter = detect_primary_key_filter();
  if (nullptr == id_filter) return false;
  
  // 提取常量值
  int64_t id_limit = extract_limit_value(id_filter);
  
  // 阈值：如果 id_limit < 10000，使用主键路径
  // 理由：10000 * 平均token数(~50) = 50万行正向索引扫描
  //      仍远小于倒排索引的百万级扫描
  const int64_t PRIMARY_KEY_THRESHOLD = 10000;
  
  return id_limit > 0 && id_limit < PRIMARY_KEY_THRESHOLD;
}
```

### 3.2 新增迭代器类

```cpp
// 文件: ob_das_primary_key_first_iter.h
class ObDASPrimaryKeyFirstIter : public ObDASIter {
public:
  // 初始化
  int init(
    const int64_t pk_limit,           // 主键上限 (如 1000)
    const ObArray<ObString> &tokens,  // 查询 tokens
    const ObArray<ObString> &base_ids,// base_id 列表 (可选)
    ObTableScanParam *main_table_scan,
    ObTableScanParam *fwd_idx_scan
  );
  
  // 核心执行
  int inner_get_next_rows(int64_t &count, int64_t capacity) override;

private:
  // 阶段1：主键扫描
  int scan_primary_keys(int64_t limit);
  
  // 阶段2：正向索引匹配
  int match_tokens_via_forward_index(
    const ObDocIdExt &doc_id,
    const ObArray<ObString> &query_tokens,
    double &relevance
  );
  
  // 阶段3：相关度计算
  int calculate_bm25_score(
    const ObArray<ObString> &doc_tokens,
    const ObArray<ObString> &query_tokens,
    double &score
  );
  
private:
  int64_t pk_limit_;
  ObArray<ObString> query_tokens_;
  ObArray<ObDocIdExt> candidate_docs_;  // 主键扫描结果
  ObTableScanParam *main_scan_param_;
  ObTableScanParam *fwd_scan_param_;
  ObDASScanIter *main_scan_iter_;
  ObDASScanIter *fwd_scan_iter_;
};
```

### 3.3 核心实现

#### 3.3.1 主键扫描

```cpp
int ObDASPrimaryKeyFirstIter::scan_primary_keys(int64_t limit) {
  int ret = OB_SUCCESS;
  
  // 设置主表扫描范围：id ∈ [1, limit]
  ObNewRange pk_range;
  ObObj range_objs[4];
  range_objs[0].set_int(1);           // start_key: id=1
  range_objs[1].set_int(limit);       // end_key: id=limit
  
  pk_range.start_key_.assign(&range_objs[0], 1);
  pk_range.end_key_.assign(&range_objs[1], 1);
  pk_range.border_flag_.set_inclusive_start();
  pk_range.border_flag_.set_inclusive_end();
  
  main_scan_param_->key_ranges_.reset();
  if (OB_FAIL(main_scan_param_->key_ranges_.push_back(pk_range))) {
    LOG_WARN("failed to push back primary key range", K(ret));
  }
  
  // 执行扫描
  int64_t batch_size = 100;
  int64_t count = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(main_scan_iter_->get_next_rows(count, batch_size))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    } else {
      // 保存 doc_id 和 base_id
      for (int64_t i = 0; i < count; ++i) {
        ObDocIdExt doc_id;
        // 从结果中提取 id 和 base_id
        // ... 
        candidate_docs_.push_back(doc_id);
      }
    }
  }
  
  return ret;
}
```

#### 3.3.2 正向索引 Token 匹配

```cpp
int ObDASPrimaryKeyFirstIter::match_tokens_via_forward_index(
    const ObDocIdExt &doc_id,
    const ObArray<ObString> &query_tokens,
    double &relevance) {
  int ret = OB_SUCCESS;
  
  // 生成正向索引扫描范围: (doc_id, MIN) → (doc_id, MAX)
  ObNewRange fwd_range;
  ObObj fwd_objs[4];
  
  if (OB_FAIL(doc_id.get_datum().to_obj(fwd_objs[0], doc_id_col_meta_))) {
    LOG_WARN("failed to set doc id", K(ret));
  } else {
    fwd_objs[1].set_min_value();  // token start
    fwd_objs[2] = fwd_objs[0];     // same doc_id
    fwd_objs[3].set_max_value();   // token end
    
    fwd_range.start_key_.assign(fwd_objs, 2);
    fwd_range.end_key_.assign(&fwd_objs[2], 2);
    fwd_range.border_flag_.set_inclusive_start();
    fwd_range.border_flag_.set_inclusive_end();
  }
  
  // 扫描正向索引
  fwd_scan_param_->key_ranges_.reset();
  if (OB_FAIL(fwd_scan_param_->key_ranges_.push_back(fwd_range))) {
    LOG_WARN("failed to push fwd range", K(ret));
  } else if (OB_FAIL(fwd_scan_iter_->rescan())) {
    LOG_WARN("failed to rescan forward index", K(ret));
  }
  
  // 收集该文档的所有 token
  ObArray<ObString> doc_tokens;
  int64_t count = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(fwd_scan_iter_->get_next_rows(count, 100))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    } else {
      // 提取 token 并保存
      for (int64_t i = 0; i < count; ++i) {
        ObString token = extract_token_from_result(i);
        doc_tokens.push_back(token);
      }
    }
  }
  
  // Token 匹配：计算交集
  int64_t match_count = 0;
  for (int64_t i = 0; i < query_tokens.count(); ++i) {
    if (doc_tokens中包含(query_tokens.at(i))) {
      match_count++;
    }
  }
  
  // 简化相关度：匹配 token 数量
  relevance = static_cast<double>(match_count);
  
  // 或调用完整 BM25 算法
  // if (OB_FAIL(calculate_bm25_score(doc_tokens, query_tokens, relevance))) ...
  
  return ret;
}
```

### 3.4 集成到 ObDASTRMergeIter

```cpp
// 在 ObDASTRMergeIter::inner_init 中
int ObDASTRMergeIter::inner_init(ObDASIterParam &param) {
  int ret = OB_SUCCESS;
  
  // ... 原有初始化 ...
  
  // 检测是否使用主键优先路径
  if (should_use_primary_key_path()) {
    use_primary_key_first_path_ = true;
    
    // 创建主键优先迭代器
    if (OB_FAIL(create_primary_key_first_iter())) {
      LOG_WARN("failed to create primary key first iter", K(ret));
    }
  } else {
    // 使用原有倒排索引路径
    if (OB_FAIL(create_dim_iters())) {
      LOG_WARN("failed to create dim iters", K(ret));
    }
  }
  
  return ret;
}

// 在 inner_get_next_rows 中
int ObDASTRMergeIter::inner_get_next_rows(int64_t &count, int64_t capacity) {
  if (use_primary_key_first_path_) {
    return primary_key_first_iter_->get_next_rows(count, capacity);
  } else {
    return sparse_retrieval_iter_->get_next_rows(count, capacity);
  }
}
```

---

## 四、分阶段实施计划

### Phase 1: 基础框架（1-2天）
- [ ] 创建 `ObDASPrimaryKeyFirstIter` 类
- [ ] 实现主键扫描逻辑
- [ ] 实现条件检测 `should_use_primary_key_path()`

### Phase 2: 正向索引集成（2-3天）
- [ ] 实现正向索引范围扫描
- [ ] 实现 Token 匹配逻辑
- [ ] 简化版相关度计算

### Phase 3: 完整相关度（2-3天）
- [ ] 集成 BM25 算法
- [ ] 支持 TF-IDF 等其他算法
- [ ] 性能优化：批量扫描、缓存

### Phase 4: 集成和测试（1-2天）
- [ ] 集成到 `ObDASTRMergeIter`
- [ ] 添加路径选择开关
- [ ] 性能测试和对比

### Phase 5: 优化（持续）
- [ ] 优化正向索引批量扫描
- [ ] 添加更多触发条件（如 base_id）
- [ ] 支持更复杂的查询模式

---

## 五、预期效果

### 5.1 性能提升

| 指标 | 当前 | 优化后 | 提升 |
|-----|------|-------|-----|
| 扫描行数 | 178万 | ~3000 | **99.8%↓** |
| 扫描 Block | 3740 | ~22 | **99.4%↓** |
| QPS | 32 | **100-200** | **3-6倍** |
| 延迟 | 32ms | **5-10ms** | **3-6倍** |

### 5.2 适用场景

**高效场景**：
- ✅ 主键范围小（id < 10000）
- ✅ base_id 过滤覆盖主键范围
- ✅ Token 数量适中（< 100）

**不适用场景**：
- ❌ 主键范围大（id < 100万）
- ❌ 无主键过滤条件
- ❌ 纯全文检索查询

### 5.3 兼容性

- ✅ 完全向后兼容
- ✅ 可通过开关控制
- ✅ 不影响现有查询

---

## 六、风险和缓解

### 6.1 风险

1. **正向索引数据量大**：如果单个文档 token 数过多（> 1000），正向索引扫描开销增加
2. **内存占用**：批量扫描需要缓存 doc_id 和 token
3. **相关度算法差异**：简化算法可能影响排序质量

### 6.2 缓解措施

1. **动态阈值调整**：根据正向索引密度调整 `PRIMARY_KEY_THRESHOLD`
2. **流式处理**：避免一次性加载所有数据
3. **保留完整算法**：支持切换到精确 BM25 计算

---

## 七、总结

这个方案通过**改变查询执行顺序**（主键优先 → 正向索引匹配 → 相关度计算），避免了倒排索引的大规模扫描。核心优势：

1. **扫描量减少 99.8%**：从 178万行降到 ~3000行
2. **base_id 过滤免费**：主键范围天然覆盖
3. **正向索引充分利用**：变被动查询为主动扫描
4. **实现难度适中**：主要是新增路径，不破坏现有逻辑

建议优先实施 Phase 1-2，快速验证性能提升，然后逐步完善。
