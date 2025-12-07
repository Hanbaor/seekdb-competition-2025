# BM25S 预计算索引设计文档

## 1. 概述

BM25S 是一个针对全文搜索优化的预计算索引模块，在 DDL 创建全文索引时预先计算 BM25 分数，并构建 CSC (Compressed Sparse Column) 格式的稀疏矩阵索引，以支持高效的全文检索查询。

### 1.1 设计目标

- **查询性能**：预计算 BM25 分数，查询时直接累加，无需实时计算
- **内存效率**：使用分桶文件流式处理，避免构建阶段 OOM
- **并发安全**：支持 DDL 并行任务收集数据
- **范围过滤**：支持 `doc_id < limit` 的高效二分剪枝

### 1.2 核心文件

| 文件 | 说明 |
|------|------|
| `ob_fts_bm25s_index.h` | 头文件，类定义和模板实现 |
| `ob_fts_bm25s_index.cpp` | 实现文件 |
| `ob_das_domain_utils.cpp` | 数据收集入口（DDL 时调用） |
| `ob_table_scan_op.cpp` | 并行任务管理和 finalize 触发 |

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    ObFtsBM25SIndex (全局单例)                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                      收集阶段 (DDL 时)                            │   │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────────┐  │   │
│  │  │ collect_token_  │  │  unique_docs_   │  │ bucket_files_    │  │   │
│  │  │ to_id_ (HashMap)│  │   (HashSet)     │  │ [128个桶文件]     │  │   │
│  │  │  token→token_id │  │  文档去重       │  │  分桶存储记录     │  │   │
│  │  └────────┬────────┘  └────────┬────────┘  └────────┬─────────┘  │   │
│  │           │                    │                    │            │   │
│  │           └────────────────────┼────────────────────┘            │   │
│  │                                ↓                                 │   │
│  │                    ┌───────────────────────┐                     │   │
│  │                    │   finalize() 排序     │                     │   │
│  │                    │   构建 CSC 索引       │                     │   │
│  │                    └───────────┬───────────┘                     │   │
│  └────────────────────────────────┼─────────────────────────────────┘   │
│                                   ↓                                     │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                      查询阶段 (SELECT 时)                         │   │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────────┐  │   │
│  │  │   token_to_id_  │  │    scores_[]    │  │   doc_ids_[]     │  │   │
│  │  │    (HashMap)    │  │  预计算BM25分数  │  │   文档ID数组      │  │   │
│  │  └─────────────────┘  └─────────────────┘  └──────────────────┘  │   │
│  │                       ┌─────────────────┐                        │   │
│  │                       │  token_ptr_[]   │                        │   │
│  │                       │  CSC列指针      │                        │   │
│  │                       └─────────────────┘                        │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 数据结构

### 3.1 收集阶段数据

```cpp
// 文件记录（20字节，紧凑存储）
struct FileRecord {
    uint32_t token_id;   // 4 字节
    int64_t doc_id;      // 8 字节
    int32_t tf;          // 4 字节 - 词频
    int32_t doc_len;     // 4 字节 - 文档长度
} __attribute__((packed));

// 收集阶段成员
FILE *bucket_files_[128];              // 分桶临时文件
TokenIdMap collect_token_to_id_;       // token → token_id
DocIdSet unique_docs_;                 // 文档去重
volatile int64_t total_doc_len_;       // 总文档长度（原子）
volatile int64_t raw_entry_count_;     // 总条目数（原子）
```

### 3.2 CSC 索引结构

```cpp
// 最终索引成员
ObArray<float> scores_;        // 预计算 BM25 分数
ObArray<uint32_t> doc_ids_;    // 文档 ID（每个 token 内按 doc_id 升序）
ObArray<uint64_t> token_ptr_;  // CSC 列指针（token_ptr_[i] 到 token_ptr_[i+1] 是 token_i 的范围）
TokenIdMap token_to_id_;       // 查询时用的 token → token_id 映射
```

### 3.3 CSC 存储格式

```
token_ptr_:  [0,    47,    150,   300,   ...]
              ↓     ↓      ↓      ↓
              token_0  token_1  token_2
              
scores_:     [s0, s1, ..., s46 | s47, s48, ..., s149 | s150, ...]
doc_ids_:    [d0, d1, ..., d46 | d47, d48, ..., d149 | d150, ...]
              \_____________/   \________________/
                  token_0           token_1
```

---

## 4. 并发控制

### 4.1 细粒度锁设计

为了支持 DDL 并行任务高效收集数据，采用细粒度锁：

```cpp
// 三把独立的读写锁/自旋锁
SpinRWLock token_map_lock_;           // HashMap 读写锁
SpinRWLock doc_set_lock_;             // DocSet 读写锁
ObSpinLock bucket_locks_[128];        // 每个桶一把锁
```

### 4.2 collect_raw_data 流程

```cpp
int collect_raw_data(token, doc_id, tf, doc_len) {
    // 第一步：获取或分配 token_id
    {
        SpinRLockGuard rguard(token_map_lock_);  // 先读锁查找
        if (token 不存在) need_insert = true;
    }
    if (need_insert) {
        SpinWLockGuard wguard(token_map_lock_);  // 写锁插入
        // 双重检查后分配新 token_id
    }
    
    // 第二步：写入桶文件
    bucket_id = token_id % 128;
    {
        ObSpinLockGuard guard(bucket_locks_[bucket_id]);  // 只锁对应桶
        fwrite(record, bucket_files_[bucket_id]);
    }
    ATOMIC_AAF(&raw_entry_count_, 1);  // 原子计数
    
    // 第三步：统计唯一文档
    {
        SpinRLockGuard rguard(doc_set_lock_);  // 先读锁检查
        if (doc 不存在) need_insert = true;
    }
    if (need_insert) {
        SpinWLockGuard wguard(doc_set_lock_);  // 写锁插入
        ATOMIC_AAF(&total_doc_len_, doc_len);
    }
}
```

### 4.3 并行任务管理

```cpp
volatile int64_t active_collectors_;  // 活跃收集任务数

void begin_collect() { ATOMIC_AAF(&active_collectors_, 1); }
bool end_collect()   { return ATOMIC_SAF(&active_collectors_, 1) == 0; }
bool can_finalize()  { return ATOMIC_LOAD(&active_collectors_) == 0; }
```

触发点（ob_table_scan_op.cpp）：

```cpp
// 任务开始时
if (!bm25s_task_started && g_bm25s_index.is_inited()) {
    g_bm25s_index.begin_collect();
    bm25s_task_started = true;
}

// 任务结束时 (OB_ITER_END)
if (bm25s_task_started) {
    bm25s_task_started = false;
    if (g_bm25s_index.end_collect()) {  // 所有任务完成
        g_bm25s_index.finalize();       // 触发 finalize
    }
}
```

---

## 5. 分桶流式处理

### 5.1 设计目标

避免收集阶段和排序阶段的内存峰值。

### 5.2 分桶策略

```
收集阶段：
  token_id % 128 → bucket_files_[bucket_id]
  
  bucket_0.bin: token_id ∈ {0, 128, 256, ...}
  bucket_1.bin: token_id ∈ {1, 129, 257, ...}
  ...
  bucket_127.bin: token_id ∈ {127, 255, 383, ...}
```

### 5.3 finalize 流程

```
1. 关闭所有桶文件
2. 第一遍扫描：统计每个 token 的 df（文档频率）
3. 计算 token_ptr_（前缀和）
4. 预分配 scores_[] 和 doc_ids_[]
5. 第二遍扫描：逐桶处理
   - 读取桶文件到内存（~20MB/桶）
   - 桶内排序（按 token_id, doc_id）
   - 计算 BM25 分数，填充到 CSC 数组
   - 释放内存
6. 删除临时桶文件
7. 清理收集阶段数据
```

### 5.4 内存效率

| 阶段 | 传统方式 | 分桶方式 |
|------|----------|----------|
| 收集 | HashMap 全量 | HashMap + 文件流 |
| 排序 | 全量加载 (~2.6GB) | 逐桶加载 (~20MB) |
| 峰值 | ~3GB | ~1.2GB |

---

## 6. BM25 分数计算

### 6.1 公式

```
IDF (Query Weight):
  diff = max(0, N - df)
  idf = log((diff + 0.5) / (df + 0.5))
  query_weight = max(ε, idf) × (1 + k1)

TF (Document Weight):
  norm_len = doc_len / avg_doc_len
  doc_weight = tf / (tf + k1 × (1 - b + b × norm_len))

Score:
  score = query_weight × doc_weight
```

### 6.2 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| k1 | 1.2 | 词频饱和参数 |
| b | 0.75 | 长度归一化参数 |
| ε | 0.25 | IDF 下界 |

---

## 7. 查询接口

### 7.1 接口定义

```cpp
int search(
    const ObIArray<ObString> &tokens,  // 查询词列表
    int64_t id_limit,                   // doc_id < id_limit 过滤（0 表示不过滤）
    int64_t top_k,                      // 返回前 K 个结果
    ObIArray<DocScore> &results         // 输出结果
);
```

### 7.2 查询流程

```cpp
1. 对每个查询 token：
   token_id = token_to_id_[token]
   start = token_ptr_[token_id]
   end = token_ptr_[token_id + 1]
   
   // 二分剪枝 doc_id < id_limit
   if (id_limit > 0) {
       valid_end = binary_search(doc_ids_, start, end, id_limit)
   }
   
   // 累加分数
   for (i = start; i < valid_end; i++) {
       doc_scores[doc_ids_[i]] += scores_[i]
   }

2. 收集所有结果，partial_sort 取 Top-K

3. 返回 (doc_id, score) 列表
```

### 7.3 复杂度

| 操作 | 复杂度 |
|------|--------|
| Token 查找 | O(1) |
| 范围二分 | O(log k) |
| 分数累加 | O(m)，m = 满足条件的文档数 |
| Top-K 排序 | O(n log k) |

---

## 8. 调试支持

### 8.1 持久化日志

```cpp
// 日志文件位置
/data/obcluster/bm25s_debug.log

// 日志格式
[2025-12-06 01:48:58][tid=140203632079616] FINALIZE_START: total_docs=200000, ...
```

### 8.2 输出文件

| 文件 | 说明 |
|------|------|
| `bm25s_csc_index.txt` | CSC 索引 dump（调试用） |
| `bm25s_finalize_time.txt` | 构建统计报告 |

### 8.3 统计报告示例

```
# BM25S Finalize Time Report
#
# Statistics:
#   total_docs: 200000
#   avg_doc_len: 1498.14
#   vocab_size: 2907746
#   total_entries: 130268282
#
# Time:
#   elapsed_ms: 13978.1
#
# Memory Usage:
#   CSC total: 1016.05 MB
#   Total (est): 1160.25 MB
```

---

## 9. 性能数据

### 9.1 测试环境

- 数据集：MLDR-en，200,000 文档
- 平均文档长度：1498 tokens

### 9.2 构建性能

| 指标 | 值 |
|------|-----|
| 总文档数 | 200,000 |
| 词汇量 | 2,907,746 |
| 总条目数 | 130,268,282 |
| 收集阶段 | ~10 分钟 |
| Finalize 阶段 | ~14 秒 |

### 9.3 内存占用

| 结构 | 大小 |
|------|------|
| scores_[] | 497 MB |
| doc_ids_[] | 497 MB |
| token_ptr_[] | 22 MB |
| token_to_id_ | ~144 MB |
| **总计** | **~1.16 GB** |

---

## 10. 代码入口

### 10.1 数据收集入口

```cpp
// ob_das_domain_utils.cpp: generate_fulltext_word_rows()
if (is_fts_index_aux && real_doc_id_for_cache >= 0) {
    g_bm25s_index.collect_raw_data(token_str, real_doc_id_for_cache, word_cnt, doc_length);
}
```

### 10.2 Finalize 触发

```cpp
// ob_table_scan_op.cpp: fetch_next_fts_index_rows()
if (OB_ITER_END == ret) {
    if (bm25s_task_started && g_bm25s_index.end_collect()) {
        g_bm25s_index.finalize();
    }
}
```

---

## 11. 后续优化方向

1. **按分数预排序**：支持高效 Top-K 查询
2. **mmap 存储**：大规模数据集内存优化
3. **增量更新**：支持文档新增/删除
4. **压缩存储**：减少内存占用

---

## 12. 版本历史

| 日期 | 版本 | 变更 |
|------|------|------|
| 2025-12-06 | v1.0 | 初始版本，支持分桶流式构建、并行收集、CSC 索引、范围查询 |
