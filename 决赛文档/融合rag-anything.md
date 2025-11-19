











### 现有体系概览（结合差异点）

1. **本地 RAG 基线**  
   - `main.py` 仅做两件事：`add()` 把 PDF 数据写入 OceanBase 混合索引表，`search()` 对用户问题做一次向量+FTS 混合检索并调用 LLM 生成答案，整体完全是“离线建库 + 在线单轮问答”流程。@rag/main.py#L1-L104  
   - `add.py` 的数据处理链固定在 “PDF→图片→VLM 提取→文本切分→OpenAI 兼容 embedding→OceanBase VECTOR+FTS 存储”，强调使用 pyobvector 的 HybridSearch 与 OceanBase 表结构。@rag/src/core/add.py#L1-L110  
   - `search.py` 直接利用 pyobvector 做 Hybrid 查询，取 TOP_K 文本拼上下文，再走通用的 `QUERY_SYSTEM_PROMPT/USER_PROMPT` + LLM。@rag/src/core/search.py#L1-L159  

2. **RAG-Anything 能力**  
   - `RAGAnything` 内建 LightRAG，提供文档解析 (MinerU/Docling)、纯文本插入、多模态条目处理（图片/表格/公式等）全流程，并维护上下文窗口、KV 缓存和批处理等高级能力。@RAG-Anything-main/raganything/raganything.py#L1-L344  
   - 查询侧支持纯文本/多模态问题、缓存、多种检索模式（local/global/hybrid/mix）以及 VLM 增强等控制项。@RAG-Anything-main/raganything/query.py#L1-L252  
   - Processor/Batch 组件提供异步并发、进度追踪和多模态 chunk 管线，可复用或裁剪纳入现有系统。@RAG-Anything-main/raganything/processor.py#L1416-L1720 @RAG-Anything-main/raganything/batch.py#L1-L206  

---

### 融合路线图（建议分阶段执行）

1. **Phase 0 – 基础对齐**
   1. 梳理依赖：补齐 RAG-Anything 所需的 LightRAG/MinerU/Docling/async 生态包，确认与当前 `rag/requirements.txt` 共存策略（可拆成独立虚拟环境或子模块）。  
   2. 定义接口层：确定 OceanBase 仍为主存储，LightRAG 只负责解析+chunk 生成，还是直接沿用 LightRAG 的本地存储；若需共存，提前规划抽象接口（如统一 `StorageAdapter`）。

2. **Phase 1 – 数据摄取融合**
   1. 复用 `RAGAnything.process_document_complete`，但在 Stage 3/4 hook 出“chunk 回调”，将 LightRAG 生成的文本/多模态描述写入 OceanBase 表结构（兼容现有 `TABLE_NAME` 列）。  
   2. 若仍需 OceanBase VLM 内容：确保 RAG-Anything 解析出的 multimodal 描述中携带 `filename/page` 等元数据，以便存储层写回。  
   3. 同步/替换现有 `add()`：可保留原流程作 fallback，新增 `add_rag_anything()` 支持高级解析，再由 CLI 选择。

3. **Phase 2 – 检索与推理联动**
   1. 方案 A：继续使用 OceanBase HybridSearch → 仅把 RAG-Anything 多模态描述当作普通文本；  
      方案 B：复用 LightRAG 的 Query 流程，改写为“先在 OceanBase 召回→再交给 LightRAG QueryParam rerank”或相反。  
   2. 统一 Prompt：在原 `QUERY_SYSTEM_PROMPT` 中加入多模态说明，或根据请求类型切换 RAG-Anything 的 prompt 配置。  
   3. 如果保留 LightRAG 查询，需要桥接当前 `Answer` 数据结构与 LightRAG 返回格式，确保输出仍含 `question/answer/filename/page` 字段。

4. **Phase 3 – 多模态问答与在线 API**
   1. 对接 RAG-Anything `aquery_with_multimodal`：扩展 REST/CLI 层允许上传图像/表格等附件，调用其 multimodal processor，再回写答案。  
   2. 若目标是“本地赛题 + rag-anything 联动”，可在 OceanBase 检索结果中记录 multimodal chunk 的 `type` & `payload`，查询时自动注入 VLM 增强路径。

5. **Phase 4 – 运营化 & 评估**
   1. 新增评测脚本（可扩展 `rag/eval.py`）对比原始 pipeline 与增强 pipeline 的准确率/延迟/成本，方便赛事或上线决策。  
   2. 完善配置管理：把 RAG-Anything 的大量参数（parser/多模态开关/batch 并发等）写入 `.env` 或独立 YAML，便于快速切换。  
   3. 补充日志与监控：围绕解析耗时、LightRAG 初始化状态、OceanBase 写入成功率等关键指标打点。

---

### 下一步建议
1. 先在独立分支尝试 Phase 0/1，将 RAG-Anything 作为 “解析器模块” 接入 `add()`。  
2. 评估 LightRAG 存储与 OceanBase 并存方案，决定最终检索栈。  
3. 规划测试集与基准，验证多模态贡献，再逐步开放到查询接口。