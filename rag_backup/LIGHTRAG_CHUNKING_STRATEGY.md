# LightRAG 分块与图谱构建策略深度解析

## 🔍 核心问题：LightRAG如何权衡chunk的使用？

通过源码分析，LightRAG的处理流程如下：

---

## 📊 完整处理流程

### 阶段1：文本分块（Chunking）

```python
# 默认配置
chunk_token_size = 1200          # 每个chunk最多1200 tokens
chunk_overlap_token_size = 100   # chunk之间重叠100 tokens
```

**示例**：假设VLM输出一整页内容（2841字符 ≈ 3000 tokens）

```
原始文本 (3000 tokens)
    ↓
LightRAG自动分块 (chunking_by_token_size)
    ↓
┌──────────────────────────────────────┐
│ Chunk 1: tokens 0-1200               │  
│ (内容: 标题 + 前几段)                │
└──────────────────────────────────────┘
        ↓ overlap 100 tokens
┌──────────────────────────────────────┐
│ Chunk 2: tokens 1100-2300            │
│ (内容: 中间段落 + 表格)              │
└──────────────────────────────────────┘
        ↓ overlap 100 tokens
┌──────────────────────────────────────┐
│ Chunk 3: tokens 2200-3000            │
│ (内容: 最后几段 + 总结)              │
└──────────────────────────────────────┘
```

**关键点**：
- ✅ **所有chunk都会被使用** - 不是选择性使用
- ✅ **有重叠区域** - 确保边界信息不丢失
- ✅ **自动切分** - 无论输入是整页还是预分块

---

### 阶段2：实体关系提取（Entity Extraction）

**每个chunk独立提取实体和关系**：

```python
async def _process_single_content(chunk_key, chunk_data):
    """处理单个chunk"""
    content = chunk_data["content"]
    
    # 1. 提取实体和关系
    entities, relations = extract_entities_and_relations(content)
    
    # 示例输出
    entities = {
        "千味央厨": [{"entity_type": "公司", "description": "速冻米面供应商..."}],
        "肯德基": [{"entity_type": "公司", "description": "连锁餐饮企业..."}],
        ...
    }
    
    relations = {
        ("千味央厨", "肯德基"): [{"description": "供应关系，自2002年合作..."}],
        ...
    }
    
    return entities, relations
```

**处理结果**：

| Chunk | 提取实体数 | 提取关系数 | 日志示例 |
|-------|-----------|-----------|---------|
| Chunk 1 | 8 | 12 | `Chunk 1 of 3 extracted 8 Ent + 12 Rel chunk-abc123` |
| Chunk 2 | 6 | 10 | `Chunk 2 of 3 extracted 6 Ent + 10 Rel chunk-def456` |
| Chunk 3 | 5 | 8 | `Chunk 3 of 3 extracted 5 Ent + 8 Rel chunk-ghi789` |

**关键点**：
- ✅ **每个chunk都独立处理** - 并发提取
- ✅ **可能有重复实体** - 重叠区域会导致同一实体在多个chunk中出现
- ✅ **保留所有提取结果** - 后续合并阶段统一处理

---

### 阶段3：实体和关系合并（Merging）

这是**LightRAG的核心权衡策略**！

#### 3.1 收集阶段

```python
# 收集所有chunk的结果
all_nodes = defaultdict(list)
all_edges = defaultdict(list)

for maybe_nodes, maybe_edges in chunk_results:
    for entity_name, entities in maybe_nodes.items():
        all_nodes[entity_name].extend(entities)
    
    for edge_key, edges in maybe_edges.items():
        all_edges[edge_key].extend(edges)
```

**示例**：
```python
all_nodes = {
    "千味央厨": [
        {"description": "Chunk1描述：速冻米面供应商...", "chunk_id": "chunk-abc123"},
        {"description": "Chunk2描述：深耕餐饮渠道...", "chunk_id": "chunk-def456"},
        {"description": "Chunk3描述：大客户资源优势...", "chunk_id": "chunk-ghi789"},
    ],
    "肯德基": [
        {"description": "Chunk1描述：连锁餐饮...", "chunk_id": "chunk-abc123"},
        {"description": "Chunk2描述：全球知名品牌...", "chunk_id": "chunk-def456"},
    ],
    ...
}
```

#### 3.2 合并策略

**实体合并**（`_merge_nodes_then_upsert`）：

```python
# 1. 去重描述
descriptions = []
for entity_data in all_entity_data:
    if entity_data.get("description"):
        descriptions.append(entity_data["description"])

# 去除重复
description_list = list(dict.fromkeys(descriptions))  # 保持顺序的去重

# 2. 决定是否需要LLM总结
if len(description_list) > 1:
    # 多个描述 -> 使用LLM总结合并
    final_description = await llm_summarize(description_list)
else:
    # 单个描述 -> 直接使用
    final_description = description_list[0]
```

**关键参数**：
- `max_source_ids_per_entity`: 每个实体最多关联多少个chunk（默认10）
- `source_ids_limit_method`: 
  - `KEEP`: 保留最早的chunk
  - `FIFO`: 保留最新的chunk

**关系合并**（`_merge_edges_then_upsert`）：

```python
# 1. 收集所有关系描述
all_relation_descriptions = [
    "Chunk1: 千味央厨自2002年与肯德基合作",
    "Chunk2: 双方建立了长期供应关系",
    "Chunk3: 千味央厨是肯德基的主要供应商之一",
]

# 2. 去重
unique_descriptions = list(dict.fromkeys(all_relation_descriptions))

# 3. LLM总结（如果描述数量超过阈值）
if should_use_llm_summary(unique_descriptions):
    final_relation_desc = await llm_summarize(unique_descriptions)
else:
    final_relation_desc = GRAPH_FIELD_SEP.join(unique_descriptions)
```

#### 3.3 日志示例

```
Phase 1: Processing 15 entities from doc-xxx (async: 8)
Merged: `千味央厨`~`肯德基` | 2+1 (dd 1)
LLMmrg: `千味央厨`~`华莱士` | 1+2
```

**日志解读**：
- `Merged`: 简单合并，未使用LLM
- `LLMmrg`: 使用LLM进行总结合并
- `2+1`: 原有2个描述 + 新增1个描述
- `dd 1`: 去重了1个重复描述

---

## ⚖️ LightRAG的权衡策略

### 1. **所有chunk都参与图谱构建**

```python
# ✅ 正确理解
所有chunk → 独立提取实体/关系 → 合并去重 → 构建图谱

# ❌ 错误理解
部分chunk被选中 → 提取实体/关系 → 构建图谱
```

### 2. **Chunk重叠的作用**

```
Chunk 1:  [--------1200 tokens--------]
Chunk 2:              [--------1200 tokens--------]
                  ↑ 重叠100 tokens

目的：
- 避免实体被切断
- 捕获跨边界的关系
- 提供上下文连贯性
```

**示例**：
```markdown
Chunk 1 结尾: ...千味央厨与肯德基建立了长期...
Chunk 2 开头: ...长期合作关系，自2002年以来...

重叠区域确保"千味央厨-肯德基"关系被完整捕获
```

### 3. **去重和总结机制**

| 场景 | 处理方式 | 示例 |
|------|---------|------|
| 相同描述 | 去重保留一个 | 3个chunk都说"千味央厨是供应商" → 保留1个 |
| 不同描述 | LLM总结合并 | Chunk1:"供应商", Chunk2:"深耕渠道" → LLM总结 |
| 描述过多 | 限制数量 | 超过10个chunk → 只保留前/后10个 |

### 4. **chunk数量限制**

```python
max_source_ids_per_entity = 10  # 每个实体最多关联10个chunk

# 如果实体出现在15个chunk中
原始: [chunk1, chunk2, ..., chunk15]
     ↓
限制策略 KEEP: [chunk1, chunk2, ..., chunk10]  # 保留最早的
限制策略 FIFO: [chunk6, chunk7, ..., chunk15]  # 保留最新的
```

**权衡考虑**：
- ✅ **控制实体描述长度** - 避免无限增长
- ✅ **保留最相关信息** - 优先级策略
- ⚠️ **可能丢失部分信息** - 超过限制的chunk被忽略

---

## 🎯 对VLM整页输入的影响分析

### 情景A：VLM整页输出（当前实现）

```
VLM输出：第1页完整内容 (3000 tokens)
    ↓
LightRAG分块：
  - Chunk 1-1: 0-1200 tokens
  - Chunk 1-2: 1100-2300 tokens  
  - Chunk 1-3: 2200-3000 tokens
    ↓
每个chunk提取实体/关系
    ↓
合并构建图谱
```

**优势**：
- ✅ 页面语义完整，chunk切分不会破坏页面结构
- ✅ 实现简单，VLM直接输出
- ✅ 重叠机制确保跨chunk的实体/关系不丢失

**劣势**：
- ⚠️ 无法区分内容来源（不知道chunk来自第几页）
- ⚠️ 如果页面很大，可能产生较多chunk

### 情景B：预先细粒度分块

```
VLM输出：
  - 段落1 (300 tokens)
  - 段落2 (400 tokens)
  - 表格1 (200 tokens)
  - ...
    ↓
LightRAG处理：
  - 检查每个块大小
  - 如果 > 1200 tokens，再次切分
  - 如果 < 1200 tokens，直接使用
    ↓
提取实体/关系
    ↓
合并构建图谱
```

**优势**：
- ✅ 可以保留段落、表格等元数据
- ✅ 更好的溯源能力
- ✅ 可以针对不同类型内容做特殊处理

**劣势**：
- ⚠️ 实现复杂，需要智能分块逻辑
- ⚠️ 可能在不恰当位置切分，破坏语义
- ⚠️ chunk数量可能过多，影响合并效率

---

## 💡 核心结论

### 1. **图谱粒度不受输入影响**

无论是整页输入还是预分块，LightRAG都会：
- 自动调整到1200 tokens的chunk大小
- 对每个chunk提取实体和关系
- 合并去重构建最终图谱

**图谱质量主要取决于**：
- ✅ VLM提取的内容质量（是否准确、完整）
- ✅ Chunk重叠参数设置（避免边界信息丢失）
- ✅ LLM总结合并的质量（描述是否清晰）

### 2. **权衡关键参数**

| 参数 | 默认值 | 作用 | 调整建议 |
|------|--------|------|---------|
| `chunk_token_size` | 1200 | chunk大小 | 根据文档复杂度调整，简单文档可增大 |
| `chunk_overlap_token_size` | 100 | 重叠区域 | 增大可减少边界丢失，但增加计算量 |
| `max_source_ids_per_entity` | 10 | 实体chunk限制 | 重要实体可能需要更多chunk，建议15-20 |
| `source_ids_limit_method` | KEEP | 限制策略 | FIFO适合持续更新的文档 |

### 3. **推荐策略**

**当前场景（金融研报等）：保持VLM整页输出**

理由：
1. ✅ 研报结构相对规整，整页语义完整
2. ✅ LightRAG自动分块足够智能
3. ✅ 实现简单稳定，易于调试
4. ✅ chunk重叠机制已经解决边界问题

**可选优化**（如需要更好的溯源）：
1. 在VLM输出中添加页面标记：`<!-- PAGE 1 -->`
2. 在VLM输出中添加段落锚点：`[#段落1]`
3. 提取时保留这些标记，便于追溯

---

## 📊 实际测试数据

基于测试的千味央厨PDF第1页：

```
原始输入：2841字符 ≈ 3000 tokens
    ↓
预估分块：
  - Chunk 1: 1200 tokens (标题 + 前几段 + 部分投资建议)
  - Chunk 2: 1200 tokens (投资建议 + 财务数据表格)
  - Chunk 3: 600 tokens (剩余内容 + 分析师信息)
    ↓
预估实体：15-20个
  - 公司：千味央厨、肯德基、华莱士、海底捞...
  - 产品：油条、蒸煎饺、烘焙类...
  - 指标：CAGR、市占率、EPS...
    ↓
预估关系：20-30个
  - 千味央厨-肯德基：供应关系
  - 千味央厨-油条：生产关系
  - 市占率-50%：数值关系
  ...
```

**合并后图谱**：
- 去重后实体：≈12-15个（重叠区域的重复实体被合并）
- 去重后关系：≈15-20个
- LLM总结：对出现在多个chunk中的实体进行描述合并

---

## 🔧 调优建议

### 场景1：文档很长（>100页）

```python
# 增大chunk大小，减少总chunk数
chunk_token_size = 1800
chunk_overlap_token_size = 150

# 增加实体chunk限制
max_source_ids_per_entity = 20
```

### 场景2：实体关系复杂

```python
# 增大重叠，确保关系完整
chunk_overlap_token_size = 200

# 保留更多chunk用于实体描述
max_source_ids_per_entity = 15
```

### 场景3：追求处理速度

```python
# 减小chunk大小，增加并发
chunk_token_size = 800
max_source_ids_per_entity = 5

# 提高并发数
llm_model_max_async = 16
```

---

## 📝 总结

**LightRAG的智慧在于**：
1. 不是"选择哪些chunk"，而是"如何合并所有chunk的信息"
2. 通过重叠确保信息不丢失
3. 通过去重避免信息冗余
4. 通过LLM总结提炼核心信息
5. 通过限制策略控制规模

**对于VLM整页输入**：
- ✅ 完全可行且推荐
- ✅ 图谱质量不受影响
- ✅ 实现简单稳定
- ⚠️ 可以通过添加标记增强溯源能力
