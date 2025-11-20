# RAGAnything 重构整体计划（适用于 `rag_backup/` 目录）

> 目标：在 `rag_backup/` 内重建 RAGAnything + docling 的完整 RAG 流程，保持 `main.py`/`eval.py` 参数接口与输出格式不变，最终替换旧 `src` 逻辑。实现分三阶段推进。

---

## 阶段一：架构对齐 & 环境准备

1. **环境变量梳理**（保持命名一致）：
   - `OPENAI_API_KEY`, `OPENAI_BASE_URL`：LLM/Embedding 调用。
   - `OUTPUT_DIR`：RAGAnything 的 `working_dir`/缓存目录（默认为 `./rag_storage`）。
   - `PARSER`：固定写 `docling`，但变量名不变；`PARSE_METHOD=auto`。
   - `OCEANBASE_*`：暂不使用，保留兼容性。
   - （可选）`EMBEDDING_MODEL`, `EMBEDDING_DIM`：自定义 embedding 模型。
2. **文档输出**：集成本计划与后续实施细节，确保任何人可按文档执行。
3. **目录规划**：
   ```
   rag_backup/
   ├── main.py              # CLI 保持不变，改为 async pipeline
   ├── eval.py
   ├── RAGAnything_Refactor_Plan.md
   ├── src/
   │   ├── config.py
   │   ├── anyrag_pipeline/
   │   │   ├── builder.py
   │   │   ├── document_loader.py
   │   │   ├── query_engine.py
   │   │   ├── answer_formatter.py
   │   │   └── utils.py
   │   └── util/           # 复用/迁移原工具
   └── data/
   ```
4. **依赖策略**：不再通过 `pip` 安装 `raganything`/`lightrag`，直接从本地仓库拷贝所需模块到 `src/anyrag_pipeline/`；`requirements.txt` 仅保留 docling 及必要依赖。

**交付物**：本计划文档 + `.env`（设置 `PARSER=docling`）、确认旧 `src` 已清空。

---

## 阶段二：Pipeline 框架搭建

1. **Config 层**：实现 `src/config.py`，负责读取 env 并构造 `RAGAnythingConfig`，提供 `load_config()` 接口。
2. **Pipeline 模块**：
   - `builder.py`：封装 RAGAnything 初始化（LLM/VLM/Embedding 函数），引用本地拷贝的 `raganything`/`lightrag` 代码。
   - `document_loader.py`：实现 `async process_dataset(rag, dataset_dir, rebuild=False)`，管理 working_dir 缓存。
   - `query_engine.py`：提供 `async ask(question)`（或 `answer_question`）接口，内部调用 `rag.aquery(..., mode="hybrid")`。
   - `answer_formatter.py`：解析 RAG 返回的 evidence，输出 `Answer(question, answer, filename, page)`。
   - `utils.py`：缓存检测、重建标记、日志辅助。
3. **模块验证**：使用单个 PDF + 少量问题做 smoke test，确保解析与检索可运行（即使答案不完善）。

**交付物**：上述 Python 模块及其本地依赖，确保 `pytest` 或简单脚本可调用。

---

## 阶段三：主流程集成与验证

1. **`main.py` 改造**：
   - 改为 `asyncio.run(main_async(args))`。
   - `main_async` 步骤：
     1. `config = load_config()`
     2. `pipeline = build_pipeline(config)`
     3. 判断是否需要重建索引（`--rebuild-index` 或缓存缺失），调用 `process_dataset`
     4. 读取题目，逐题 `await query_engine.ask()`
     5. 通过 `answer_formatter` 得到标准 `Answer`
     6. `write_answers` 写入输出文件
2. **功能校验**：
   - 跑 `python main.py --dataset ./data/dataset --questions ./data/questions.json --output ./data/output.json`
   - 再跑 `python eval.py --output ./data/output.json --answer ./data/answer.json` 验证格式无误。
3. **文档更新**：在本计划基础上补充实施结果、注意事项、性能数据等。

**交付物**：可运行的主流程（含缓存机制）、更新后的说明文档、必要的日志/测试记录。

---

## 说明

- 每个阶段结束前，需在本文件中记录完成情况与关键输出，方便后续跟踪。
- 若 docling 在当前环境存在依赖问题，可在阶段二记录替代方案（如 fallback OCR），但仍需保持接口一致。
