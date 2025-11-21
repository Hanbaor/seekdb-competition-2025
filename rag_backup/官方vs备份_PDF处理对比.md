# 官方 vs rag_backup PDF处理方法对比

## 📊 核心架构对比

| 维度 | 官方代码 (`rag/`) | rag_backup |
|------|------------------|------------|
| **框架** | 纯OceanBase + 自研RAG | LightRAG + RAGAnything |
| **存储** | OceanBase向量数据库 | LightRAG (NetworkX图 + 向量) |
| **并发模型** | ThreadPoolExecutor（文件级） | AsyncIO Queue（页级） |
| **图谱** | ❌ 无 | ✅ 实体关系图谱 |

---

## 🔍 PDF处理流程对比

### 官方代码（rag/src/core/add.py）

```python
# 1. 文件级并发
with ThreadPoolExecutor(max_workers=4) as executor:
    for pdf_file in pdf_files:
        executor.submit(process_pdf_file, pdf_file, client)

# 2. 单PDF内部：串行处理每一页
def process_pdf_file(pdf_file, client):
    for page_num in range(1, total_pages + 1):
        # Step 1: PDF页 → 图片
        page_image = pdf_page_to_image(pdf_file, page_num, dpi=100)
        
        # Step 2: 图片 → VLM解析
        page_text = generate_vlm_response(
            VLM_IMAGE_EXTRACTION_PROMPT,
            images=page_image
        )
        
        # Step 3: 文本分块
        chunks = split_text(page_text)
        
        # Step 4: 每个chunk独立处理
        for chunk in chunks:
            embedding = generate_embedding(chunk)
            client.insert({
                "filename": filename,
                "page": page_num,
                "content": chunk,
                "vector": embedding
            })
        
        # Step 5: 清理临时图片
        os.unlink(page_image)

# 优点：
# ✅ 简单直接，易理解
# ✅ 页面立即清理，内存占用低
# ✅ 出错不影响其他PDF

# 缺点：
# ❌ 页内串行，速度慢（30页PDF = 30次顺序VLM调用）
# ❌ VLM workers未充分利用（4个PDF × 串行 = 低并发）
# ❌ 无图谱，只有向量+全文检索
```

### rag_backup（document_loader.py + parser.py）

```python
# 1. 全局页面级任务队列 + 生产者-消费者模式
async def process_dataset(rag, dataset_dir):
    # Phase 1: 批量转换所有PDF → 图片
    pdf_images = {}
    for pdf in pdfs:
        images = convert_from_path(pdf, dpi=150)
        pdf_images[pdf] = [(page_num, image_path), ...]
    
    # Phase 2: 创建全局任务队列
    task_queue = asyncio.Queue()
    for pdf, pages in pdf_images.items():
        for page_num, image_path in pages:
            await task_queue.put((pdf, page_num, image_path))
    # 示例：19个PDF × 30页 = 570个任务在队列中
    
    # Phase 3: VLM Worker Pool（页级并发）
    vlm_worker_count = 20  # 20个worker并发处理
    
    async def worker(worker_id):
        while not task_queue.empty():
            pdf, page_num, image_path = await task_queue.get()
            
            # 调用VLM解析单页
            content = await vlm_analyze_page(image_path, page_num)
            
            # 存储到临时结果
            page_results[pdf][page_num] = content
            
            # 检查PDF是否完成
            if len(page_results[pdf]) == total_pages[pdf]:
                await completed_pdfs.put(pdf)  # 通知图谱构建器
    
    # 启动20个VLM worker
    workers = [asyncio.create_task(worker(i)) for i in range(20)]
    
    # Phase 4: 图谱构建 Worker Pool（PDF级并发）
    graph_worker_count = 5  # 5个worker并发构建图谱
    
    async def graph_builder(worker_id):
        while True:
            pdf = await completed_pdfs.get()  # 等待完成的PDF
            
            # 合并该PDF的所有页面
            content_list = [page_results[pdf][p] for p in sorted_pages]
            combined_text = "".join(item["text"] for item in content_list)
            
            # LightRAG插入（实体关系提取 + 图谱构建）
            await rag.lightrag.ainsert(
                input=combined_text,
                file_paths=pdf.name,
                ids=doc_id
            )
    
    # 启动5个图谱构建worker
    graph_builders = [asyncio.create_task(graph_builder(i)) for i in range(5)]
    
    # 等待所有worker完成
    await asyncio.gather(*workers, *graph_builders)

# 优点：
# ✅ 极高并发（20个VLM worker并行处理不同PDF的页面）
# ✅ 流水线架构（VLM解析 + 图谱构建并行）
# ✅ 图谱增强检索（实体+关系）
# ✅ 充分利用API并发限制

# 缺点：
# ⚠️ 内存占用较高（所有图片预先转换）
# ⚠️ 复杂度高，调试难度大
# ⚠️ 出错影响面较大
```

---

## 🔢 性能对比（19个PDF，平均30页/PDF）

### 官方代码

```python
# 假设：
# - VLM调用时间：3秒/页
# - Embedding时间：0.5秒/chunk
# - 每页3个chunk

时间线（max_workers=4）：
├─ PDF1 Worker: 30页 × 3秒 = 90秒（串行）
├─ PDF2 Worker: 30页 × 3秒 = 90秒（串行）
├─ PDF3 Worker: 30页 × 3秒 = 90秒（串行）
├─ PDF4 Worker: 30页 × 3秒 = 90秒（串行）
└─ PDF5-19: 排队等待...

总时间 = 19 PDFs ÷ 4 workers × 90秒 = 427.5秒 ≈ 7分钟

实际瓶颈：
- 单PDF内部完全串行
- 4个worker无法充分利用API并发限制（假设限制=50 QPS）
```

### rag_backup

```python
时间线（VLM=20 workers, Graph=5 workers）：
Phase 1: 图片转换（并行）
  ├─ 19个PDF × 30页 = 570页
  └─ 时间：~2分钟（I/O密集，可并行）

Phase 2: VLM解析（20个worker并发）
  ├─ 570个页面任务
  ├─ 20个worker并发处理
  └─ 时间：570 ÷ 20 × 3秒 = 85.5秒 ≈ 1.5分钟

Phase 3: 图谱构建（与Phase 2并行）
  ├─ 5个worker并发构建
  ├─ 每个PDF图谱构建：~60秒
  └─ 时间：19 PDFs ÷ 5 × 60秒 = 228秒 ≈ 3.8分钟

总时间（流水线）：
  max(Phase 2, Phase 3) = 3.8分钟

加速比：7分钟 ÷ 3.8分钟 = 1.84倍
```

---

## 📦 数据存储对比

### 官方代码（OceanBase表结构）

```sql
CREATE TABLE rag_documents (
    id INTEGER PRIMARY KEY AUTO_INCREMENT,
    source_id VARCHAR(64),
    filename VARCHAR(512),      -- PDF文件名
    page INTEGER,                -- 页码
    content VARCHAR(8192),       -- 文本内容
    vector VECTOR(1024)          -- 向量
);

-- 索引
CREATE VECTOR INDEX vec_idx ON rag_documents(vector);
CREATE FULLTEXT INDEX fts_idx_content ON rag_documents(content);

-- 检索方式
SELECT filename, page, content
FROM rag_documents
WHERE MATCH(content) AGAINST('千味央厨成立时间')  -- 全文检索
  AND vector <-> query_vector < 0.7              -- 向量检索
ORDER BY score DESC
LIMIT 5;
```

**特点**：
- ✅ chunk级存储（一页可能多个chunk）
- ✅ 页码精确
- ❌ 无实体关系
- ❌ 无图谱

### rag_backup（LightRAG图谱 + 向量）

```python
# 1. 文本内容 → LightRAG
await rag.lightrag.ainsert(
    input=combined_text,  # 全文
    file_paths=pdf.name,
    ids=doc_id
)

# LightRAG内部存储：
# a) 实体提取
entities = {
    "千味央厨": {
        "type": "ORG",
        "description": "速冻食品企业",
        "source_docs": ["doc-abc123"]
    },
    "2012年": {
        "type": "TIME",
        "description": "成立时间",
        "source_docs": ["doc-abc123"]
    }
}

# b) 关系提取
relations = {
    ("千味央厨", "成立于", "2012年"): {
        "weight": 0.95,
        "source": "千味央厨成立于2012年",
        "source_docs": ["doc-abc123"]
    },
    ("千味央厨", "客户", "海底捞"): {
        "weight": 0.92,
        ...
    }
}

# c) NetworkX图
G = nx.Graph()
G.add_edge("千味央厨", "2012年", relation="成立于", weight=0.95)
G.add_edge("千味央厨", "海底捞", relation="客户", weight=0.92)

# d) 向量存储（chunk级）
chunks = [
    {"text": "千味央厨成立于2012年...", "embedding": [...], "doc_id": "doc-abc123"},
    {"text": "主要客户包括海底捞...", "embedding": [...], "doc_id": "doc-abc123"},
]

# 检索方式（混合）
# 路径1：图谱检索
query_entities = extract_entities("千味央厨成立时间")  # [千味央厨, 成立时间]
neighbors = G.neighbors("千味央厨")  # 找相关实体
paths = nx.shortest_path(G, "千味央厨", "2012年")  # 找路径

# 路径2：向量检索
vector_results = semantic_search(query_embedding, chunks)

# 路径3：实体过滤
filtered = [r for r in vector_results if "千味央厨" in r.entities]
```

**特点**：
- ✅ 图谱增强检索
- ✅ 实体+关系语义理解
- ✅ 多跳推理能力
- ⚠️ 页码需要额外维护（当前未实现）

---

## 🎯 针对比赛的优缺点

### 官方代码优势

1. **✅ 天然适配评分规则**
   ```python
   # 每个chunk都有精确的(filename, page)
   result = {
       "filename": "千味央厨-XX.pdf",
       "page": 15,  # ← 直接返回，不需要映射
       "content": "..."
   }
   ```

2. **✅ 简单可靠**
   - 代码量少，易调试
   - 出错影响小

3. **✅ 内存占用低**
   - 即时清理临时文件

### 官方代码劣势

1. **❌ 速度慢**
   - 页内串行处理
   - 并发度低（4个PDF worker）

2. **❌ 无图谱**
   - 只有向量+全文检索
   - 无法做多跳推理
   - 定位准确率可能较低

3. **❌ 无单页摘要**
   - 检索结果可能冗长
   - 答案生成质量一般

### rag_backup优势

1. **✅ 速度快**
   - 页级并发（20 workers）
   - 流水线架构
   - 加速1.8倍+

2. **✅ 图谱增强**
   - 实体+关系提取
   - 多跳推理
   - 定位准确率可能更高

3. **✅ 架构先进**
   - 生产者-消费者模式
   - 异步并发

### rag_backup劣势

1. **❌ 复杂度高**
   - 调试困难
   - 维护成本高

2. **⚠️ 页码信息可能丢失**
   ```python
   # 当前实现
   await rag.lightrag.ainsert(
       input=combined_text,  # 全文合并
       file_paths=pdf.name,
       ids=doc_id
   )
   # 问题：页码信息在合并时丢失！
   ```

3. **⚠️ 内存占用高**
   - 所有图片预先加载

---

## 💡 融合方案建议

### 方案A：官方代码 + 页级并发优化

```python
# 在 add.py 中修改
def add(dataset_dir, max_worker):
    pdf_files = glob.glob(os.path.join(dataset_dir, "*.pdf"))
    
    # ========== 改进1：创建全局页面任务队列 ==========
    all_page_tasks = []
    for pdf_file in pdf_files:
        total_pages = len(PdfReader(pdf_file).pages)
        for page_num in range(1, total_pages + 1):
            all_page_tasks.append((pdf_file, page_num))
    
    # ========== 改进2：页级并发 ==========
    def process_single_page(pdf_file, page_num, client):
        """处理单页"""
        page_image = pdf_page_to_image(pdf_file, page_num, dpi=100)
        page_text = generate_vlm_response(PROMPT, images=page_image)
        chunks = split_text(page_text)
        
        for chunk in chunks:
            embedding = generate_embedding(chunk)
            client.insert({
                "filename": os.path.basename(pdf_file),
                "page": page_num,
                "content": chunk,
                "vector": embedding
            })
        
        os.unlink(page_image)
    
    # 使用更大的并发数
    with ThreadPoolExecutor(max_workers=20) as executor:  # 从4改到20
        futures = [
            executor.submit(process_single_page, pdf, page_num, client)
            for pdf, page_num in all_page_tasks
        ]
        
        for future in as_completed(futures):
            future.result()

# 效果：
# - 保持简单架构
# - 速度提升5倍（4→20并发）
# - 仍然天然适配评分规则
```

### 方案B：官方代码 + LightRAG图谱（推荐）

```python
# 在 add.py 中增加图谱构建
def add(dataset_dir, max_worker):
    # Phase 1: VLM解析 + 向量存储（保持原有）
    # ...
    
    # ========== Phase 2：图谱构建（新增） ==========
    from lightrag import LightRAG
    
    lightrag = LightRAG(
        working_dir="./graph_db",
        llm_model_func=llm_func,
        embedding_func=embedding_func
    )
    
    # 为每个PDF构建图谱
    for pdf_file in pdf_files:
        # 从数据库读取该PDF的所有内容
        pdf_name = os.path.basename(pdf_file)
        results = client.query(
            f"SELECT page, content FROM rag_documents 
             WHERE filename = '{pdf_name}' ORDER BY page"
        )
        
        # 按页组织内容
        pages_content = {}
        for row in results:
            page_num = row['page']
            if page_num not in pages_content:
                pages_content[page_num] = []
            pages_content[page_num].append(row['content'])
        
        # 每页内容合并后插入LightRAG
        for page_num, chunks in pages_content.items():
            page_text = " ".join(chunks)
            
            # 插入图谱（附带元数据）
            await lightrag.ainsert(
                input=page_text,
                metadata={
                    "filename": pdf_name,
                    "page": page_num  # ← 保留页码！
                }
            )

# 效果：
# - 既有向量+全文检索（官方）
# - 又有图谱增强（rag_backup）
# - 页码信息不丢失
```

### 方案C：rag_backup + 页码保留修复

```python
# 修改 document_loader.py
async def graph_builder(worker_id):
    pdf_path = await completed_pdfs.get()
    
    # 获取排序后的页面
    sorted_pages = sorted(page_results[pdf_path].keys())
    content_list = [page_results[pdf_path][p] for p in sorted_pages]
    
    # ========== 改进：逐页插入，保留页码 ==========
    for page_num, content_entry in zip(sorted_pages, content_list):
        await rag.lightrag.ainsert(
            input=content_entry["text"],
            metadata={  # ← 新增metadata支持
                "filename": pdf_path.name,
                "page": page_num,
                "pdf_path": str(pdf_path)
            },
            ids=f"{doc_id}_page_{page_num}"
        )

# 效果：
# - 保持高速并发架构
# - 保留页码信息
# - 图谱增强检索
```

---

## 🏆 最终建议

### 对于比赛：

**方案B（官方 + LightRAG图谱）** ⭐⭐⭐⭐⭐

理由：
1. ✅ 基于官方代码，风险低
2. ✅ 增加图谱检索，提升定位准确率（+15-20分）
3. ✅ 保持页码精确性
4. ✅ 实施简单（只需加一个Phase 2）
5. ⏱️ 时间成本可控（3-5天实现）

实施优先级：
1. **立即**：优化官方VLM prompt（已完成）
2. **今天**：实现方案A（页级并发优化）
3. **明天**：实现方案B（集成LightRAG图谱）
4. **2-3天**：优化search.py定位逻辑
5. **可选**：单页摘要生成

需要我帮你实现方案A或B吗？
