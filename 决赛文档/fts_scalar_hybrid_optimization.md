# seekdb 全文索引 + 标量过滤优化建议

## 1. 当前执行链路概览

| 模块 | 关键代码 | 现状摘要 |
| ---- | -------- | -------- |
| 表达式生成 | `ObTableFtsExprCgService::generate_text_retrieval_dep_exprs()` @/home/tian/seekdb/src/observer/table/fts/ob_table_fts_cg_service.cpp#71-214 | 为全文检索准备 `MATCH`、BM25 相关表达式以及 token/doc 统计列，全程在执行期动态构建，缺少缓存。 |
| doc_id merge | `ObTableFtsDmlCgService::generate_scan_with_doc_id_ctdef_if_need()` @/home/tian/seekdb/src/observer/table/fts/ob_table_fts_cg_service.cpp#466-509 | 生成 `ObDASDomainIdMergeCtDef`，依赖 `rowkey_doc` 表把 doc_id 映射回主表 rowkey。 |
| DAS 迭代器 | `ObDASIterUtils::create_domain_id_scan_sub_tree()` + `ObDASDomainIdMergeIter` @/home/tian/seekdb/src/sql/das/iter/ob_das_iter_utils.cpp#1750-1840、@/home/tian/seekdb/src/sql/das/iter/ob_das_domain_id_merge_iter.cpp#70-259 | 运行期需要并行维护数据表与 `rowkey_doc` 的扫描迭代器，每次 rescan/批次都要重建扫描参数。 |
| 结果回填 | `ObDASDomainIdMergeIter::get_and_fill_domain_id_in_data_table()` @/home/tian/seekdb/src/sql/das/iter/ob_das_domain_id_merge_iter.cpp#987-1116 | 将 `rowkey_doc` 读到的 doc_id 再写回主表 `result_output_`，存在额外内存拷贝。 |

典型带标量过滤查询：
```sql
SELECT docid_col, MATCH(fulltext_col) AGAINST(? ) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST(? )
  AND base_id IN (...)
  AND id < ...
ORDER BY _score DESC
LIMIT 10;
```
执行流程：倒排扫描 `fts_index` → 通过 `rowkey_doc` 获取主表 rowkey → doc_id merge 连接主表 → 读取 `base_id`/`id` 做过滤 → 排序/limit。

## 2. 架构痛点

1. **doc_id merge 成本高**：`rowkey_doc` 仅包含 doc_id ↔ rowkey，merge 迭代器需要逐行匹配两个表，CPU/IO 压力大 [@/home/tian/seekdb/src/sql/das/iter/ob_das_domain_id_merge_iter.cpp#897-1248]。
2. **标量过滤无法下推**：倒排阶段不了解 `base_id`/`id` 条件，必须回表后再过滤，导致大量无效回表。
3. **表达式构建重复**：`generate_text_retrieval_dep_exprs()` 每次请求都重新创建 `ObRawExpr`/`ObMatchFunRawExpr`，缺乏 ctx 级别缓存 [@/home/tian/seekdb/src/observer/table/fts/ob_table_fts_cg_service.cpp#71-214]。
4. **迭代器初始化开销大**：`ObDASDomainIdMergeIter::inner_init()` 每轮扫描重新分配 `ObTableScanParam`、range 等对象，无法复用 [@/home/tian/seekdb/src/sql/das/iter/ob_das_domain_id_merge_iter.cpp#165-215]。
5. **结果回填链条长**：doc_id 回填需要多次 `ObString` 拷贝 [@/home/tian/seekdb/src/sql/das/iter/ob_das_domain_id_merge_iter.cpp#987-1116]。
6. **top-K 早停缺失**：倒排扫描没有基于当前 top-K 的终止条件，带 limit 的查询仍然拉取大量候选。 

## 3. 可落地优化方案

### 3.1 FTS 辅助表结构增强

| 改动 | 说明 |
| ---- | ---- |
| rowkey_doc 冗余标量列 | 在 `ObFtsIndexBuildTask::construct_rowkey_doc_arg()` 中允许配置额外列（如 `base_id`、`id`），构建索引时直接写入，使 doc_id merge 可在 rowkey_doc 层完成标量过滤。 |
| fts_index/fts_doc_word 冗余标量 | 在 `prepare_aux_index_tables()` 构建 schema 时传入用户指定的标量列，倒排扫描可直接读取并做粗过滤，减少下游回表数量。 |
| schema 描述扩展 | `ObDDLService::create_aux_index()` 需支持额外列定义，`ObComplementDataParam`/数据补全过程负责填充。 |

### 3.2 doc_id merge 执行链路优化

1. **多 doc_id 批量 range**：在 `ObDASDomainIdMergeIter::build_rowkey_domain_range()` 内按批合并 key range，减少 rescan 次数。
2. **迭代器复用**：缓存 `rowkey_domain_scan_params_`，通过 `ObDASDomainIdMergeIter::inner_reuse()` 只更新 tablet/ls 即可，避免频繁 new/delete。
3. **批量回填**：使用 `fill_batch_domain_id_datum()` 一次性写入多行 doc_id，降低 `memcpy` 次数。
4. **并行 merge**：针对高基数 doc_id，引入多条 merge iter 并行消费（可按 tablet/LS 维度拆分）。

### 3.3 表达式构建缓存

| 动作 | 细节 |
| ---- | ---- |
| ctx cache | 在 `ObTableCtx` 中增加 `text_retrieval_expr_cache`，`generate_text_retrieval_dep_exprs()` 首次构建后放入 cache，命中直接复用。 |
| formalize 延后 | 仅在 cache miss 时执行 `formalize()`，热路径避免重复。 |d
| 常量折叠 | 将 `search_keywords`、limit/offset 等常量表达式提前折叠，减少 runtime formalize。 |

### 3.4 标量过滤下推策略

1. **Bloom Filter**：构建索引时为 `fts_index` 生成 `base_id` 的 Bloom Filter，小成本过滤明显不匹配的 doc_id。
2. **虚拟列下推**：在 `ObTableFtsExprCgService::generate_match_against_exprs()` 中为 `MATCH` 表达式附带过滤谓词，使倒排扫描能够早期剔除 rowkey。
3. **Top-K Early Stop**：在倒排引擎维护当前 top-K `score` 阈值，低于阈值的 doc_id 直接丢弃。

### 3.5 DAS & 查询执行提升

- `ObDASIterUtils::create_domain_id_scan_sub_tree()` 支持动态选择“轻量回表路径”（当 rowkey_doc 已具备所需列时，跳过主表访问）。
- `ObDASScanOp::reuse_iter()` 中提前设置 domain merge 相关的 tablet 信息，减少 `set_domain_id_merge_related_ids()` 的锁竞争。 
- 文本检索 lookup (`append_fts_relavence_project_col`) 支持将关联度列直接注入结果，省掉额外 `push_back`。 

### 3.6 BM25 评分优化

BM25 这块同样有不少空间可以挖，方向主要集中在**统计精度、预计算和执行路径**三类：

1. **统计数据更精确/更丰富**  
   - 目前在 [generate_text_retrieval_dep_exprs()](cci:1://file:///home/tian/seekdb/src/observer/table/fts/ob_table_fts_cg_service.cpp:70:0-216:1) 中构造 `related_doc_cnt`、`total_doc_cnt`、`doc_token_cnt` 等表达式时，是基于辅助表里已有的计数值直接做 BM25 计算 [@/home/tian/seekdb/src/observer/table/fts/ob_table_fts_cg_service.cpp#71-214]。可以考虑在索引构建阶段就维护更精确的 `doc_length` 分布（例如记录平均值、方差），或者针对不同 `base_id` 分 buckets，查询时选用更贴合条件的 k1/b 参数。

2. **预计算/缓存 BM25 中间量**  
   - 在 `ObFtsIndexBuildTask::prepare_aux_index_tables()` 阶段为倒排表增加“预计算 BM25 权重”列（例如存储 `idf(term)`），查询时只需做与 `tf` 相关的少量浮点运算；这与现有架构兼容，仍然是辅助表的一部分。  
   - 针对热门查询或高频 term，可以在内存中维护 `idf` 结果的小型缓存（仍属于执行器内部结构，不会违反 README 的“禁止新增缓存系统”约束）。

3. **执行期优化**  
   - 目前 BM25 相关表达式是在 `ObRawExpr` 层动态构建并逐行求值，可考虑把 BM25 计算下沉到 C++ 层的批处理函数，使用向量化或 SIMD 提升吞吐。  
   - 当 LIMIT 较小（Top-N 查询）时，可以在倒排扫描阶段维护一个 min-heap，结合 BM25 分数早停，减少大量低分文档的 BM25 计算和回表成本。

这些改动都属于“在现有 BM25 计算链路内做增强”，不需要引入新的系统级缓存或额外组件，与当前架构约束兼容。
近年来在搜索引擎/数据库社区里，围绕 BM25 提升 QPS 的思路大致集中在以下几类：

1. **量化（Quantization）+ LUT**  
   - 例如在倒排索引中将 `idf(term)` 或整段 BM25 分值量化成 8bit/16bit，并配合查表（Lookup Table）做近似。这样在查询时只需做整数运算或数组索引，显著降低浮点开销。学术界与工业界都有实践（如 Lucene/Splunk 的“Impact Score Quantization”），理论上能在误差可控的前提下提升 20~40% 的吞吐。

2. **Two-Stage / Cascade Ranking**  
   - 先用量化后的 BM25 或其他轻量指标做粗排，从倒排里筛出少量候选，然后对候选再用高精度 BM25（或 BM25+L2 正则）重算。这让大部分文档只经历廉价的量化评分，Top-K 才进入精算阶段。

3. **向量化 / SIMD**  
   - 把 BM25 计算下沉到 C++ 批处理，利用 SIMD 指令一次处理多条 `tf`、`doc_len`，再配合多核并行。比如 Faiss/Lucene 的部分 patch 就是通过 AVX2/AVX-512 处理 BM25 中的 `log`/`division`，QPS 有可观提升。

4. **参数自适应 + 分桶统计**  
   - 针对不同文档集或子集（如特定 `base_id`），离线统计更贴合的 `avg_doc_len`、k1/b 参数，减少运行时计算；甚至可按桶维护多个版本的量化表，查询时选取最合适的一份。

这些做法本质都是在“压缩 BM25 计算成本”的路线上：要么通过量化/查表减少浮点运算，要么在执行策略上分阶段/批处理。结合你们当前的架构（辅助表 + 倒排 + doc_id merge），可以优先考虑 Impact Score Quantization + Top-K 逐步精算这类方案，既不破坏现有执行路径，也能直接提高 QPS。


## 4. 实施步骤（建议顺序）

1. **Schema 扩展**：完善 `ObFtsIndexBuildTask::*arg()` & `ObDDLService::create_aux_index()` 以支持标量列冗余。 
2. **数据补全适配**：`ObComplementDataParam`、补全 DAG 在写 rowkey_doc/fts_index 时填充标量值。 
3. **执行链路优化**：实现 doc_id merge 批量化 & 迭代器复用，确保兼容旧 schema。 
4. **表达式缓存 & 下推**：引入 ctx cache，提供 Bloom Filter/虚拟列推送能力。 
5. **后续增强**：完成 top-K early stop、并行 merge 等进一步优化。 

## 5. 预期收益

| 优化 | 影响 |
| ---- | ---- |
| 标量冗余 + 下推 | 大幅减少回表量，降低 doc_id merge CPU/IO 开销，改善 tail latency。 |
| 批量 merge & 复用 | 减少 rescan/迭代器初始化成本，提高高并发稳定性。 |
| 表达式缓存 | 缩短执行计划准备时间，降低 CPU 占用。 |
| top-K 早停 | TOP-N 查询吞吐提升，尤其在 `LIMIT` 场景。 |

通过以上改进，可在不改变 SQL 语义的前提下，显著提升“全文 + 标量过滤”场景的 QPS 与 P99 延迟表现。

