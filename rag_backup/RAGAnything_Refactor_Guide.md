# RAGAnything 重构指导（parser = docling）

> 目标：完全替换现有 `rag/src` 实现，改用 RAG-Anything Pipeline（解析器固定为 docling），同时保持 `main.py` / `eval.py` / 环境变量命名和输出格式不变，以满足比赛评测要求。

---

## 1. 环境变量（沿用原命名）

| 变量 | 说明 | 默认/备注 |
| --- | --- | --- |
| `OPENAI_API_KEY` | LLM & Embedding API Key | 评测环境提供，RAGAnything 直接读取 |
| `OPENAI_BASE_URL` | OpenAI 兼容接口地址 | `https://dashscope.aliyuncs.com/compatible-mode/v1` |
| `OUTPUT_DIR` | 解析/缓存目录 | 作为 `working_dir`/`output` 使用，默认 `./rag_storage` |
| `PARSER` | 解析器选择 | 固定传 `docling`（但变量名保留，便于评测配置） |
| `PARSE_METHOD` | 解析方式 | `auto`（必要时切换 `ocr`/`txt`） |
| `OCEANBASE_URI/USER/PASSWORD/DBNAME` | 旧 pipeline 依赖 | 重构后不使用，但需保留以兼容外部脚本 |
| 可选：`EMBEDDING_MODEL` / `EMBEDDING_DIM` | 控制向量模型 | 默认 `text-embedding-3-large` / `3072` |

> 所有新模块通过 `os.getenv("变量名")` 读取，确保与现有 `.env` / 评测环境一致。

---

## 2. 新目录结构（示例）

```
rag/
├── main.py                 # CLI 保持不变（改为 async 调用新 pipeline）
├── eval.py
├── RAGAnything_Refactor_Guide.md
├── requirements.txt        # 加入 raganything + docling 依赖
├── src/
│   ├── config.py           # 读取 env，构造 RAGAnythingConfig
│   ├── anyrag_pipeline/
│   │   ├── builder.py           # 初始化 RAGAnything + 模型函数
│   │   ├── document_loader.py   # 批量 process_document_complete
│   │   ├── query_engine.py      # aquery / aquery_with_multimodal
│   │   ├── answer_formatter.py  # 解析结果，生成 (question, answer, filename, page)
│   │   └── utils.py             # 缓存检测、日志等
│   └── util/                    # 保留 read_questions_json / write_answers 等
└── data/
    ├── dataset/
    ├── questions.json
    └── output.json
```

原 `src/core`, `src/parser`, `src/storage` 等可删除或迁入 `rag_backup/`，确保主目录仅包含 AnyRAG 实现。

---

## 3. 工作流概述

1. **配置加载** (`src/config.py`)
   ```python
   config = RAGAnythingConfig(
       working_dir=os.getenv("OUTPUT_DIR", "./rag_storage"),
       parser=os.getenv("PARSER", "docling"),  # 固定传 docling
       parse_method=os.getenv("PARSE_METHOD", "auto"),
       enable_image_processing=True,
       enable_table_processing=True,
       enable_equation_processing=True,
   )
   ```
   - LLM / VLM / Embedding 函数同 `raganything_example.py`，读取 `OPENAI_API_KEY` / `OPENAI_BASE_URL`。

2. **文档解析 & 索引** (`document_loader.process_dataset`)
   ```python
   async def process_dataset(rag, dataset_dir, rebuild=False):
       for pdf in Path(dataset_dir).glob("*.pdf"):
           if not rebuild and cache_exists(pdf):
               continue
           await rag.process_document_complete(
               file_path=str(pdf),
               output_dir=config.working_dir,
               parse_method=config.parse_method,
           )
   ```
   - working_dir 存储解析结果与索引，重复运行可跳过。

3. **问答流程** (`query_engine.ask`)
   ```python
   async def ask(question):
       result = await rag.aquery(question, mode="hybrid")
       return result  # 包含答案文本 + evidence
   ```

4. **答案格式化** (`answer_formatter.to_answer`)
   ```python
   def to_answer(question, rag_result):
       answer_text = rag_result["answer"]
       evidence = pick_best_evidence(rag_result)
       filename = evidence.get("filename", "")
       page = evidence.get("page", 0)
       if not filename:
           answer_text = "无法从提供的资料中确定答案。"
       return Answer(question, answer_text, filename, page)
   ```
   - 若 RAGAnything 未返回 `filename/page`，需从检索到的 chunk metadata 中抽取。

5. **主程序 (`main.py`)**
   ```python
   async def main_async(args):
       rag = build_pipeline()
       if need_rebuild(args.dataset):
           await process_dataset(rag, args.dataset)
       questions = read_questions_json(args.questions)
       answers = []
       for q in questions:
           result = await ask(q)
           answers.append(to_answer(q, result))
       write_answers(answers, args.output)

   if __name__ == "__main__":
       args = parse_args()
       asyncio.run(main_async(args))
   ```

---

## 4. 实施步骤

1. **依赖更新**：`requirements.txt` 仅保留 docling 解析所需依赖；`raganything` 与 `lightrag` 代码直接从本地仓库拷贝进 `src/anyrag_pipeline/`，不再通过 pip 安装，以避免额外安装时间。
2. **复制/移植代码**：从 `/home/tian/seekdb/RAG-Anything-main/`（或已有 `rag_backup/vendor`）拷贝 `raganything`、`lightrag` 相关模块到项目 `src/`，按新结构拆分后本地引用。
3. **环境配置**：在 `.env` 中设置 `PARSER=docling`，其余变量保持原名。
4. **迭代开发**：
   - 先确认 `process_dataset` + `aquery` 能跑通单个 PDF。
   - 再接入问题列表、输出 JSON。
   - 最后 `python eval.py --output data/output.json --answer data/answer.json` 验证得分。
5. **日志与缓存**：在 `working_dir/logs` 记录耗时，必要时新增 `--rebuild-index` 参数强制重新解析。

---

## 5. 注意事项

- **输出格式**：必须保持 `question/answer/filename/page`，否则 `eval.py` 会扣分。
- **页码/文件名来源**：务必来自检索证据，不能让 LLM 随意杜撰。
- **时间限制**：利用 working_dir 缓存，避免每次解析全量 PDF。
- **API 限速**：RAGAnything 内部已有退避机制，如需可在 builder 中调整重试参数。

---

通过上述指导，可在不改变 CLI / 评测接口的前提下，用 docling + RAG-Anything 构建新的多模态 RAG 流程。只需确保环境变量命名不变、输出格式符合要求，即可顺利替换旧 `src/` 实现。
