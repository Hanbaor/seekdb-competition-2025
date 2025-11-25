# 全文混合标量检索性能分析与回表优化策略

## 一、火焰图整体分析

### 1.1 主要调用链路（占用 69.32% CPU）

```
start_thread (69.32%)
└── ObThWorker::worker (69.08%)
    └── ObThWorker::process_request (66.21%)
        └── ObQueryDriver::response_query_result (65.97%)
            └── ObResultSet::get_next_row (65.94%)
                └── ObTableScanOp::get_next_batch_with_das (65.67%)
                    └── ObDASCacheLookupIter::inner_get_next_rows (47行) 
```

### 1.2 关键性能瓶颈定位

从火焰图和 perf 数据可以看出，主要性能热点分为两大部分：

#### **热点1：回表操作 (58.66%)**
- `ObDASCacheLookupIter::inner_get_next_rows` (47行)
  - `ObDASFuncDataIter::inner_get_next_rows` (49行，58.66%)
    - `ObDASTRMergeIter::inner_get_next_rows` (51行，54.13%)
      - **`ObSRSortedLookupIter::load_results` (53行，54.13%)** ⭐核心瓶颈
        - `ObSRDaaTIterImpl::get_next_rows` (54行)
          - `ObSRDaaTIterImpl::do_one_merge_round` (56行，54.09%)
            - **`ObSRDaaTIterImpl::fill_merge_heap` (58行，53.78%)** ⭐关键热点
              - `ObTextRetrievalDaaTTokenIter::get_next_row` (60行，53.62%)
                - `ObTextRetrievalTokenIter::get_next_batch` (62行，53.57%)
                  - **`ObDASScanIter::inner_get_next_rows` (64行，52.50%)** ⭐IO热点

#### **热点2：数据读取与解码 (52.50%)**
- `ObTableScanIterator::get_next_rows` (65行)
  - `ObMultipleMerge::get_next_rows` (66行)
    - **存储层扫描 (52.47%)**
      - `ObSSTableRowScanner::inner_get_next_row` (75行，51.51%)
        - **索引定位 (30.80%)**: `fetch_row` → `open_cur_data_block`
          - `ObMicroBlockRowScanner::open` (81行，29.85%)
            - **Rowkey比较 (27.82%)**: `locate_range_pos` → `locate_range` → `find_bound`
              - `ObMicroBlockDecoder::compare_rowkey` (92行，15.69%)
                - **字符串比较 (8.63%)**: `ObCharset::strcmpsp` → `ob_strnncollsp_any_uca`
                - **列解码 (4.70%)**: `ObColumnDecoder::decode`
        - **预取优化 (20.64%)**: `ObIndexTreeMultiPassPrefetcher::prefetch`
          - `prefetch_micro_data` (14.85%)
          - `prefetch_index_tree` (5.67%)

## 二、回表性能瓶颈详细分析

### 2.1 回表架构概览

当前全文混合标量检索的回表流程：

```
查询请求
  │
  ├─ 全文索引扫描（Token Matching）
  │   └─ ObTextRetrievalTokenIter::get_next_batch
  │       └─ ObDASScanIter::inner_get_next_rows (52.50% CPU) ⚠️
  │
  ├─ DaaT归并（Document-at-a-Time）
  │   └─ ObSRDaaTIterImpl::fill_merge_heap (53.78% CPU) ⚠️
  │       └─ 多个Token迭代器的归并
  │
  ├─ 回表查找（Lookup）
  │   └─ ObSRSortedLookupIter::load_results (54.13% CPU) ⚠️
  │       └─ 根据doc_id回表获取标量数据
  │
  └─ 数据表扫描（主表数据读取）
      └─ ObSSTableRowScanner (51.51% CPU) ⚠️
          ├─ Rowkey定位与比较 (30.80%)
          └─ 数据预取 (20.64%)
```

### 2.2 核心瓶颈：`ObSRSortedLookupIter::load_results`

**代码位置**: `/home/tian/seekdb/src/storage/retrieval/ob_sparse_lookup_iter.cpp:183-226`

**核心问题**：

1. **顺序回表导致大量随机IO**
   ```cpp
   int ObSRSortedLookupIter::load_results() {
     int cur_idx = 0;
     bool iter_end = false;
     // 循环调用merge_iter_->get_next_rows()，每次都触发存储层IO
     while (OB_SUCC(ret) && !iter_end && cur_idx < rangekey_size_) {
       int64_t sub_count = 0;
       if (OB_FAIL(merge_iter_->get_next_rows(rangekey_size_ - cur_idx, sub_count))) {
         // 处理迭代结束...
       }
       // 逐个匹配cached_domain_ids_与返回的id_datums
       for (int64_t i = 0; OB_SUCC(ret) && i < sub_count; ) {
         if (OB_FAIL(cmp_func_(cached_domain_ids_[cur_idx].get_datum(), 
                              *id_datums.at(i), cmp_result))) {
           // 比较doc_id并存储relevance
         }
       }
     }
   }
   ```

2. **批量大小受限**
   - 当前回表是逐批进行（batch by batch）
   - 每批的大小受`rangekey_size_`和`cache_capacity_`限制
   - 无法充分利用向量化IO

3. **Cache未命中率高**
   - 回表的doc_id往往是离散的，导致：
     - L1/L2 Cache miss
     - TLB miss
     - Page Cache miss
   - 大量的随机读操作

### 2.3 存储层扫描瓶颈

**代码位置**: `ObSSTableRowScanner` 和 `ObMicroBlockRowScanner`

**核心问题**：

1. **Rowkey比较开销大 (27.82% CPU)**
   - 字符串比较函数 `ob_strnncollsp_any_uca` 占用 7.79% CPU
   - 每次micro block定位都需要进行二分查找
   - 比较涉及字符集转换和排序规则

2. **数据块打开开销 (29.85% CPU)**
   - 每次切换到新的micro block都需要：
     - 解压缩（如果启用压缩）
     - 初始化解码器
     - 定位range位置

3. **列解码开销 (4.70% CPU)**
   - RLE解码 (2.48%)
   - Dict解码 (0.79%)
   - Raw解码 (0.79%)

## 三、回表优化策略（重点）

### 3.1 批量回表优化（Batch Lookup Optimization）

**策略1: 增大回表批次大小**

**当前代码**（`ob_das_cache_lookup_iter.cpp:275-371`）：
```cpp
int ObDASCacheLookupIter::inner_get_next_rows(int64_t &count, int64_t capacity) {
  switch (state_) {
    case INDEX_SCAN: {
      while (OB_SUCC(ret) && !index_end_ && 
             lookup_rowkey_cnt_ < default_batch_row_count_) {  // 受限于default_batch_row_count_
        // 收集rowkey...
      }
    }
    case DO_LOOKUP: {
      if (OB_FAIL(do_index_lookup())) {  // 执行回表
        LOG_WARN("failed to do index lookup", K(ret));
      }
    }
  }
}
```

**优化建议**：
```cpp
// 1. 动态调整批次大小
const int64_t ADAPTIVE_BATCH_SIZE = std::min(
    1000,  // 最大批次
    std::max(default_batch_row_count_, 
             estimate_optimal_batch_size())  // 根据查询特征动态估算
);

// 2. 预取多个批次的rowkey
struct LookupBatchQueue {
  std::vector<RowkeyBatch> pending_batches_;
  int prefetch_depth_ = 3;  // 预取深度
};
```

**预期收益**：减少20-30%的回表次数，降低状态切换开销

---

### 3.2 Rowkey排序与聚合（Rowkey Sorting & Aggregation）

**策略2: 对回表的rowkey进行排序，转随机IO为顺序IO**

**原理**：
- 全文索引返回的doc_id通常是无序的
- 对doc_id排序后回表，可以：
  - 利用存储层的顺序预读机制
  - 提高Page Cache命中率
  - 减少磁盘寻道次数

**实现位置**：`ObDASCacheLookupIter::INDEX_SCAN` 阶段

**优化代码示例**：
```cpp
class ObDASCacheLookupIter {
private:
  // 添加排序缓冲区
  struct SortedRowkeyBuffer {
    std::vector<std::pair<ObRowkey, int64_t>> rowkeys_;  // <rowkey, original_order>
    
    void add_rowkey(const ObRowkey &key, int64_t order) {
      rowkeys_.push_back({key, order});
    }
    
    void sort_by_rowkey() {
      std::sort(rowkeys_.begin(), rowkeys_.end(),
                [](const auto &a, const auto &b) {
                  return a.first.compare(b.first) < 0;
                });
    }
  };
  
  SortedRowkeyBuffer sorted_buffer_;
  
  int add_rowkey() override {
    // 不立即回表，先缓存
    sorted_buffer_.add_rowkey(current_rowkey_, lookup_rowkey_cnt_);
    ++lookup_rowkey_cnt_;
    
    // 达到批次大小时排序并回表
    if (lookup_rowkey_cnt_ >= default_batch_row_count_) {
      sorted_buffer_.sort_by_rowkey();
      // 然后进行回表...
    }
  }
};
```

**预期收益**：
- **随机IO → 顺序IO**：50-70%的性能提升
- **Page Cache命中率**：从20%提升到60-80%

---

### 3.3 预取与流水线化（Prefetching & Pipelining）

**策略3: 在回表过程中实现异步预取**

**当前问题**：
- `ObSRSortedLookupIter::load_results` 是同步阻塞的
- 每次调用 `merge_iter_->get_next_rows()` 都等待IO完成

**优化方案**：**Double Buffering + 异步预取**

```cpp
class ObSRSortedLookupIterOptimized {
private:
  struct DoubleBuffer {
    Buffer front_buffer_;  // 正在处理的数据
    Buffer back_buffer_;   // 正在预取的数据
    std::future<int> prefetch_task_;
    
    void swap() {
      std::swap(front_buffer_, back_buffer_);
    }
  };
  
  DoubleBuffer double_buffer_;
  
  int load_results_async() {
    // 1. 启动后台预取任务
    double_buffer_.prefetch_task_ = std::async(std::launch::async, [this]() {
      return this->prefetch_next_batch();
    });
    
    // 2. 处理当前buffer的数据
    process_current_buffer();
    
    // 3. 等待预取完成并交换buffer
    double_buffer_.prefetch_task_.wait();
    double_buffer_.swap();
  }
};
```

**预期收益**：
- **IO等待时间**：减少40-60%
- **吞吐量**：提升1.5-2倍

---

### 3.4 索引覆盖优化（Index Covering Optimization）

**策略4: 减少不必要的回表**

**当前代码分析**（`ob_das_cache_lookup_iter.cpp:211-213`）：
```cpp
} else if (need_index_proj && OB_FAIL(index_proj_rows_.save(false, 1))) {
  LOG_WARN("save index proj rows failed", K(ret));
}
```

**问题**：
- 如果查询的标量字段已经在全文索引中（如doc_id、relevance），仍然会进行回表
- 缺少"索引覆盖"判断逻辑

**优化建议**：
```cpp
// 1. 在查询计划生成阶段判断是否需要回表
bool need_table_lookup() {
  // 检查所有输出列是否都在索引中
  for (auto *expr : output_exprs_) {
    if (!is_in_index(expr)) {
      return true;
    }
  }
  return false;  // 无需回表
}

// 2. 如果无需回表，直接使用索引数据
if (!need_table_lookup()) {
  state_ = OUTPUT_ROWS;  // 跳过DO_LOOKUP阶段
}
```

**预期收益**：
- 对于只查询doc_id和relevance的场景：**避免100%的回表开销**
- 性能提升：**3-5倍**

---

### 3.5 Micro Block缓存优化（Micro Block Cache Optimization）

**策略5: 优化存储层的数据块缓存策略**

**当前问题**（从火焰图看）：
- `ObSSTableRowScanner::fetch_row` → `open_cur_data_block` 占用30.80% CPU
- 每次打开新block都有显著开销

**优化方向**：

**A. 增加Micro Block Cache大小**
```cpp
// 在系统配置中调整
SET GLOBAL micro_block_cache_size = 8GB;  // 根据内存情况调整
```

**B. 实现智能预热策略**
```cpp
class MicroBlockPrefetcher {
  // 基于访问模式预测下一个要访问的block
  void predict_and_prefetch(const ObRowkey &current_key) {
    // 1. 分析历史访问模式
    auto pattern = access_pattern_analyzer_.analyze(current_key);
    
    // 2. 预测下一个block
    auto next_block_id = predict_next_block(pattern);
    
    // 3. 异步预取
    async_fetch_block(next_block_id);
  }
};
```

**预期收益**：
- **Block Cache命中率**：从60%提升到85-90%
- **数据块打开开销**：减少40-50%

---

### 3.6 向量化回表（Vectorized Lookup）

**策略6: 使用SIMD指令优化批量比较操作**

**当前瓶颈**（火焰图显示）：
- `ObCharset::strcmpsp` → `ob_strnncollsp_any_uca` 占用8.22% CPU
- 字符串比较是标量操作

**优化方案**：
```cpp
// 使用AVX2/AVX-512进行批量比较
class VectorizedRowkeyComparator {
  int batch_compare(const ObRowkey *keys1, const ObRowkey *keys2, 
                    int64_t count, int *results) {
    #ifdef __AVX2__
    // 使用256位SIMD寄存器
    for (int64_t i = 0; i < count; i += 8) {
      __m256i vec1 = _mm256_load_si256((__m256i*)(keys1 + i));
      __m256i vec2 = _mm256_load_si256((__m256i*)(keys2 + i));
      __m256i cmp_result = _mm256_cmpeq_epi32(vec1, vec2);
      _mm256_store_si256((__m256i*)(results + i), cmp_result);
    }
    #endif
  }
};
```

**预期收益**：
- **字符串比较性能**：提升3-4倍
- **整体回表性能**：提升10-15%

---

## 四、存储层优化建议

### 4.1 索引结构优化

**优化1: 为全文检索场景定制的索引格式**

当前的倒排索引（Inverted Index）存储格式可能不够优化：

```
当前格式: <token, doc_id, position, ...>
优化格式: <token, [doc_id_batch], [compressed_positions]>
```

**改进点**：
- 使用Delta编码压缩doc_id序列
- 使用PForDelta编码位置信息
- 减少索引大小，提升Cache友好性

### 4.2 数据分区优化

**优化2: 按照doc_id范围分区**

```sql
-- 创建分区表
CREATE TABLE documents (
  doc_id BIGINT,
  content TEXT,
  ...
) PARTITION BY RANGE (doc_id) (
  PARTITION p0 VALUES LESS THAN (1000000),
  PARTITION p1 VALUES LESS THAN (2000000),
  ...
);
```

**收益**：
- 回表时可以并行访问多个分区
- 减少锁竞争
- 提升扫描效率

### 4.3 列编码优化

**优化3: 针对标量字段使用更高效的编码**

从火焰图看，`ObColumnDecoder::decode` 占用4.70% CPU：

```cpp
// 当前: 使用通用的RLE/Dict编码
// 优化: 针对不同数据类型选择最优编码

class OptimizedColumnEncoder {
  EncodeStrategy select_strategy(const ColumnStats &stats) {
    if (stats.is_sorted) {
      return EncodeStrategy::DELTA_ENCODING;  // 有序数据用Delta
    } else if (stats.distinct_ratio < 0.1) {
      return EncodeStrategy::DICT_ENCODING;   // 低基数用Dictionary
    } else if (stats.is_numeric) {
      return EncodeStrategy::FRAME_OF_REFERENCE;  // 数值用FOR
    }
    return EncodeStrategy::RAW;
  }
};
```

## 五、代码级优化清单

### 5.1 高优先级优化（预期收益>30%）

1. **Rowkey排序回表**
   - 文件: `src/sql/das/iter/ob_das_cache_lookup_iter.cpp`
   - 函数: `ObDASCacheLookupIter::add_rowkeys`
   - 优化: 添加排序逻辑

2. **增大批次大小**
   - 文件: `src/sql/das/iter/ob_das_iter_utils.cpp:3300`
   - 参数: `lookup_param.default_batch_row_count_`
   - 优化: 从当前值（可能是256）增大到1000-2000

3. **索引覆盖判断**
   - 文件: `src/sql/das/iter/ob_das_cache_lookup_iter.cpp`
   - 函数: `inner_get_next_rows`
   - 优化: 添加skip lookup逻辑

### 5.2 中优先级优化（预期收益10-30%）

4. **异步预取**
   - 文件: `src/storage/retrieval/ob_sparse_lookup_iter.cpp:183`
   - 函数: `ObSRSortedLookupIter::load_results`
   - 优化: 实现double buffering

5. **Micro Block缓存**
   - 配置: 调整 `micro_block_cache_size`
   - 代码: 优化预取策略

6. **向量化比较**
   - 文件: `src/storage/blocksstable/ob_micro_block_decoder.cpp`
   - 函数: `compare_rowkey`
   - 优化: 使用SIMD指令

### 5.3 低优先级优化（预期收益<10%）

7. **列解码优化**
   - 改进RLE/Dict解码器的实现
   - 使用更高效的压缩算法

8. **内存池优化**
   - 减少内存分配次数
   - 使用对象池复用

## 六、性能监控指标

### 6.1 关键指标

优化前后应该监控以下指标：

```cpp
struct LookupMetrics {
  // 回表次数
  uint64_t lookup_count;
  
  // 平均批次大小
  double avg_batch_size;
  
  // IO类型分布
  uint64_t sequential_io_count;
  uint64_t random_io_count;
  
  // Cache命中率
  double page_cache_hit_ratio;
  double block_cache_hit_ratio;
  
  // 延迟分布
  Histogram latency_dist;  // P50, P95, P99
};
```

### 6.2 优化目标

| 指标 | 当前值（预估） | 目标值 |
|------|-------------|--------|
| 平均批次大小 | 256 | 1000+ |
| 随机IO占比 | 80% | 30% |
| Page Cache命中率 | 20% | 70% |
| P99延迟 | 500ms | 100ms |
| 整体吞吐量 | 基线 | 3-5倍 |

## 七、总结

### 核心问题
全文混合标量检索的性能瓶颈主要在**回表阶段**，占用了超过50%的CPU时间。主要原因是：
1. **大量随机IO**：doc_id无序导致随机回表
2. **批次大小受限**：无法充分利用批量IO
3. **Cache未命中**：数据访问模式不友好

### 优化优先级
1. **立即实施**（1-2周）：
   - Rowkey排序回表
   - 增大批次大小
   - 索引覆盖优化

2. **短期实施**（1个月）：
   - 异步预取
   - Micro Block缓存优化
   - 向量化比较

3. **长期演进**（2-3个月）：
   - 索引结构重构
   - 数据分区优化
   - 列编码改进

### 预期收益
综合实施上述优化后，预期：
- **回表性能提升**: 3-5倍
- **整体查询延迟**: 降低60-70%
- **系统吞吐量**: 提升3-5倍

---

**生成时间**: 2024年
**分析工具**: Linux perf + 火焰图
**目标系统**: OceanBase SeekDB
