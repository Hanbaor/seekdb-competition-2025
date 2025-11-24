# SR PRUNING 多层过滤架构

## 过滤层级说明

### LAYER 1: 默认扫描范围限制
**位置**: `ob_das_tr_merge_iter.cpp::gen_inv_idx_scan_default_range()`
**作用**: 设置倒排索引默认扫描的 end_key = 1000
**日志标识**: `SR PRUNING [LAYER 1]`
**关键代码**: 
```cpp
obj_ptr[3].set_uint64(1000);  // 限制 end_key doc_id
```

### LAYER 2: Function Lookup 精确扫描过滤
**位置**: `ob_das_tr_merge_iter.cpp::set_children_iter_rangekey(virtual_rangekeys, batch_size)`
**作用**: 在 function lookup 模式下，跳过 > 1000 的 doc_id，不为它们生成扫描范围
**日志标识**: `SR PRUNING [LAYER 2]`
**关键代码**:
```cpp
if (doc_id_val > 1000) {
  continue;  // 不生成扫描范围
}
```

### LAYER 3A: Sorted Lookup 缓存过滤
**位置**: `ob_sparse_lookup_iter.cpp::ObSRSortedLookupIter::set_hints()`
**作用**: 过滤 virtual_rangekeys，只缓存 <= 1000 的 doc_id
**日志标识**: `SR PRUNING [LAYER 3A]`
**关键代码**:
```cpp
if (doc_id_val > 1000) { continue; }
rangekey_size_ = valid_size;  // 更新为有效数量
```

### LAYER 3B: Hash Lookup 缓存过滤
**位置**: `ob_sparse_lookup_iter.cpp::ObSRHashLookupIter::set_hints()`
**作用**: 过滤 virtual_rangekeys，只缓存 <= 1000 的 doc_id
**日志标识**: `SR PRUNING [LAYER 3B]`
**关键代码**:
```cpp
if (doc_id_val > 1000) { continue; }
rangekey_size_ = valid_size;
```

### LAYER 4: 倒排索引扫描验证层
**位置**: `ob_text_retrieval_token_iter.cpp::get_next_batch()`
**作用**: 验证倒排索引扫描结果，检测是否有 > 1000 的 doc_id 泄露
**日志标识**: `SR PRUNING [LAYER 4]`
**关键代码**:
```cpp
if (doc_id_val > 1000) {
  LOG_ERROR("SCAN RANGE FILTER FAILED!");
  skip_->set(i);  // 兜底过滤
}
```

### LAYER 5: 回表查询兜底过滤
**位置**: `ob_das_func_data_iter.h::add_doc_id()`
**作用**: 最后一道防线，拒绝添加 > 1000 的 doc_id
**日志标识**: `SR PRUNING [LAYER 5]`
**关键代码**:
```cpp
if (doc_id_val > 1000) {
  LOG_WARN("LEAKAGE DETECTED!");
  return ret;  // 拒绝添加
}
```

## 日志grep命令

```bash
# 查看所有层的过滤日志
grep "SR PRUNING \[LAYER" /data/obcluster/log/observer.log | tail -100

# 查看特定层
grep "SR PRUNING \[LAYER 1\]" /data/obcluster/log/observer.log
grep "SR PRUNING \[LAYER 2\]" /data/obcluster/log/observer.log
grep "SR PRUNING \[LAYER 3A\]" /data/obcluster/log/observer.log
grep "SR PRUNING \[LAYER 3B\]" /data/obcluster/log/observer.log
grep "SR PRUNING \[LAYER 4\]" /data/obcluster/log/observer.log
grep "SR PRUNING \[LAYER 5\]" /data/obcluster/log/observer.log

# 查看泄露检测
grep "LEAKAGE DETECTED\|SCAN RANGE FILTER FAILED" /data/obcluster/log/observer.log
```

## 预期行为

**正常情况**（无泄露）:
- LAYER 1: 设置扫描范围上限 1000
- LAYER 2-3: (可能不执行，取决于查询模式)
- LAYER 4: 验证通过，无泄露
- LAYER 5: 只处理有效 doc_id

**异常情况**（有泄露）:
- LAYER 4: 报告 "SCAN RANGE FILTER FAILED"
- LAYER 5: 报告 "LEAKAGE DETECTED"
