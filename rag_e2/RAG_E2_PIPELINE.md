# RAG_E2 全流程说明

本文档详细描述 `rag_e2` 项目在比赛环境中的端到端流程：从 PDF 原始文件解析、缓存与图谱构建，到查询阶段的检索与答案生成。所有内容基于当前代码实现（2025-11-21），涉及的主要模块包括 `document_loader.py`、`vlm_parser.py`、`api_client.py`、`E2GraphRAG-main` 目录中的检索逻辑，以及 `main.py` 中的任务编排。

---

## 1. 配置入口与关键目录

- **配置文件**：`competition_config.yaml`
  - `dataset`: PDF 数据、问题与输出路径。
  - `paths`: 缓存(`cache/`)、已处理标记(`processed_documents/`)、日志等位置。
  - `cluster`: 段落切分长度、重叠、合并参数（用于 Tree/Graph 构建）。
  - `vlm`: VLM 模型与并发 worker 配置。
  - `graph`: 图构建 worker 数。
  - `retriever.kwargs`: 检索器运行参数（embedding 维度、是否使用 API embedding 等）。

- **核心目录**：
  - `cache/<dataset_name>/<document_name>/`: 存放 `vlm_results.json`、`tree.json|pkl`、`graph.json|pkl`、`index.json` 等。
  - `processed_documents/`: 以 `<pdf_name>.done` 标记文件处理完成，避免重复解析。
  - `debug_output/`: 查询阶段调试信息（按问题编号输出 JSON）。

---

## 2. 文件解析与索引构建（Ingestion Pipeline）

### 2.1 主入口：`main.py` 的 `main_async`
1. 加载配置与 `.env` 中的 API 信息。
2. 初始化 `APIClient` 和 `VLMParser`。
3. 若未指定 `--skip-ingest`，调用 `DocumentProcessor.process_dataset()` 处理 PDF。

### 2.2 PDF → 图像：`DocumentProcessor._convert_pdfs_to_images`
- 使用 `pdf2image.convert_from_path` 将每份 PDF 转为逐页 PNG。
- 输出至 `cache/<pdf_stem>/vlm/images/page_XXXX.png`。
- 若目录已存在图像则跳过转换，提高重复运行效率。

### 2.3 图像 → VLM 文本：`VLMParser.analyze_page`
- 读取 PNG，base64 编码后组合系统 + 用户 prompt。
- 通过 `APIClient.call_vlm` 调用 `qwen3-vl-flash`（默认），获取 Markdown 文本：
  - 保留原文结构、标题、表格、图片描述等。
- Worker 逻辑：`DocumentProcessor.process_dataset` 会同时运行多个 `vlm_worker`，并将结果写入 `vlm_results.json`（page_num → text）。
- 已存在完整缓存（页数一致）时直接复用，无需再次调用 VLM。

### 2.4 聚合文本并构建图谱：`DocumentProcessor.graph_worker`
1. 当某 PDF 所有页都完成解析后，将 `[PAGE_START:x] ... [PAGE_END:x]` 拼接成 `full_text`。
2. 调用 `_run_e2graph_build`：
   - 使用 `tiktoken` + `sequential_split` 按 `chunk_length/overlap` 切分。
   - 执行 `process_utils.build_tree_task` 构建分层语义树（Tree）。
   - 执行 `process_utils.extract_graph_task` 生成实体图（Graph）、倒排索引(`index.json`)及实体出现计数(`appearance_count.json`)。
3. 将 `tree.json`、`graph.json`、`index.json`、`appearance_count.json` 序列化为 `.pkl`，供查询阶段快速加载。
4. 在 `processed_documents/` 下创建 `<pdf_stem>.done` 标记。

> **注意**：图谱构建阶段仍遵循 E2GraphRAG 的原始流程，涉及 CPU 密集型 NLP 计算，`graph_worker_count` 建议不超过物理核心数。

---

## 3. 查询与回答（Query Pipeline）

### 3.1 查询入口：`main.py`
1. 读取问题列表（字符串或包含 `question` 字段的 dict）。
2. 加载 spaCy NLP（用于潜在实体处理，当前纯向量检索可忽略）。
3. 遍历 `cache/<dataset_name>/*`，为每个文档加载 `tree.pkl` 与 `graph.pkl`，构建 `Retriever` 实例：
   ```python
   retriever = Retriever(tree, G, index, appearance_count, nlp, **config["retriever"]["kwargs"])
   ```
4. 创建异步信号量，限制同时处理的问题数（`query.max_concurrent_queries`）。

### 3.2 纯向量检索：`E2GraphRAG-main/query.py::Retriever.query`
- 目前逻辑已改为 **100% FAISS 向量检索**：
  1. 调用 `dense_retrieval(query, max_chunk_setting)`，直接从每个文档的向量索引中取前 `max_chunk_setting` 个 chunk。
  2. 使用 `format_res` 拼接文本，并记录补充信息（检索类型标记为 `Pure Vector Retrieval`）。
- 该方法跳过 Graph/Tree 的实体筛选，最大化保持语义相似度排序。

### 3.3 问题处理流程：`main.py::process_one_question`
1. 对每个 `Retriever` 调用 `query()`，收集：
   - `chunks_text`: 命中的文本片段。
   - `retrieval_score`: 以 chunk 数量作为简单打分（可替换为相似度）。
   - 调试信息：文档名、检索类型、chunk IDs、实体等。
2. 将所有有结果的文档按 `(score, chunks_length)` 倒序排序，选取得分最高者构建回答上下文。
3. 构造 prompt：
   ```text
   Based on the following context, answer the question.
   If the answer is not in the context, say "I don't know".
   ```
4. 通过 `APIClient.call_vlm`（同 VLM 模型）生成答案。
5. 源追踪：
   - `scored_results[0]` 的 `doc_name` 视为主要来源。
   - 使用正则从 chunk 文本中提取原文中的 `filename`、`page`（若可用）。
6. 将答案、置信度、调试信息写入 `output.json` 与 `debug_output/question_xx.json`。

### 3.4 评估脚本
- `rag/eval.py`：对输出结果进行自动打分（基于参考答案 + LLM 相关性 + 文件/页码匹配）。
- 支持同时给出累计得分与调试日志，便于定位检索或归因问题。

---

## 4. Simple Vector Baseline（可选）
- `main_simple.py` + `simple_vector_retriever.py` 提供纯向量 baseline：
  1. 直接读取 `vlm_results.json`，拼文本、切 chunk 并使用 `text-embedding-v4` 构建 FAISS 索引。
  2. 跳过 Graph/Tree 缓存，仅比对语义向量结果。
- 目前仍在调试中（FAISS 维度判定需注意 `embedding_dim` 与 `np.ndarray` 形状）。

---

## 5. 数据流总结

```
PDFs
  │
  ├─(pdf2image)─▶ PNG 图像
  │                 │
  │                 └─(VLMParser + APIClient)─▶ 每页 Markdown 文本 → vlm_results.json
  │
  └─(DocumentProcessor.graph_worker)
         │
         ├─ 拼接页面文本 (含 PAGE 标记)
         ├─ token 切分 + build_tree_task → tree.json/pkl
         ├─ extract_graph_task → graph.json + index.json + appearance_count.json → graph.pkl
         └─ 处理完成标记 processed_documents/<doc>.done

查询阶段
  │
  ├─ main.py 加载 retriever（tree.pkl + graph.pkl）
  ├─ 对每个问题依次：
  │      ├─ Retriever.query → 纯向量检索（FAISS）返回 chunk
  │      ├─ 汇总所有文档的 chunk，按得分排序
  │      ├─ 构造上下文 + 问题 → APIClient.call_vlm 生成答案
  │      └─ 记录来源信息、调试日志
  └─ 结果写入 output.json & debug_output/
```

---

## 6. 常见问题与排查建议

1. **VLM API 报错/超时**：检查 `OPENAI_API_KEY`、`OPENAI_BASE_URL`，或降低 `vlm.worker_count`。
2. **Graph 构建阻塞**：确认 `processed_documents/<doc>.done` 是否被误删；若需要重建，使用 `--rebuild`。
3. **查询无结果**：
   - 确认 `cache/<dataset>/<doc>/graph.pkl` 是否存在。
   - 在 `debug_output/question_xx.json` 中查看 `retrieval_type`，确认是否走到 `Pure Vector Retrieval`。
4. **Simple Vector Baseline 出错**：确保 `SimpleVectorRetriever` 的 embedding 维度与模型一致（默认 1024），且 `np.array` 形状为 `(num_chunks, dim)` 再写入 FAISS。

---

## 7. 关键脚本速查

| 目的 | 文件 | 备注 |
| --- | --- | --- |
| 主流程入口 | `rag_e2/main.py` | `python main.py --config ... --questions ...` |
| PDF 解析与索引 | `rag_e2/document_loader.py` | `DocumentProcessor.process_dataset` |
| 视觉解析器 | `rag_e2/vlm_parser.py` | `VLMParser.analyze_page` |
| API 封装 | `rag_e2/api_client.py` | `APIClient.call_vlm` 支持重试 |
| 检索器核心 | `rag_e2/E2GraphRAG-main/query.py` | 当前强制 `Pure Vector Retrieval` |
| 简化向量基线 | `rag_e2/main_simple.py` | 可用于对比图谱效果 |
| 评估脚本 | `rag/eval.py` | 与官方答案对比 |

---

如需在该流程上继续调优，可重点关注：
- `Retriever.query` 中的排序与打分方式（目前基于 chunk 数量）。
- `SimpleVectorRetriever` 的索引构建逻辑，作为完全绕开图谱的基准线。
- `DocumentProcessor` 中的缓存与并发策略，保证 VLM 与图谱构建阶段的稳定性。
