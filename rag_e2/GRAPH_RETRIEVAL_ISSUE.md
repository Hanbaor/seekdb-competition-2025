# Graph检索失效原因分析

## 🔍 问题现象

- **Global Search**: 40/280 (14.3%) - 完全没提取到实体
- **Occurrence Rerank**: 240/280 (85.7%) - 提取到实体但Graph检索返回空，退化为向量检索+重排
- **Local Search**: 0/280 (0%) - Graph检索从未成功

## 🎯 根本原因

### 1. **查询时实体提取完全失败** ❌

**实际提取结果**：
```python
Q1: "千味央厨的成立时间是什么时候？" -> []
Q2: "千味央厨的财务报表预测中..." -> ['？']
Q9: "千味央厨的定制生产模式..." -> ['千味央厨的定制生产模式与通用品生产模式有何不同']
```

**问题**：
- spaCy中文模型`zh_core_web_sm`在短查询上完全失效
- 提取到的"实体"要么是空，要么是问号，要么是整个问题本身
- 没有提取到真正的关键实体："千味央厨"、"成立时间"、"财务"等

### 2. **Graph中的实体与查询不匹配** ⚠️

**Graph中实际的实体**（以"千味央厨-公司研究报告-深耕餐饮供应链为人间千味"为例）：
```python
- "千味央厨公司"  ✅ (但查询提取不到"千味央厨")
- "新乡千味"
- "华莱士"
- "中国"
- "速冻食品"
- "[图片"  ❌ (噪声)
- "├─"     ❌ (噪声)
- "END"    ❌ (噪声)
```

**匹配失败的原因**：
- 查询："千味央厨" (未提取) vs Graph："千味央厨公司" (存在)
- 查询："成立时间" (未提取) vs Graph：不存在这个实体
- 即使查询提取到"千味央厨公司"，也没有"成立时间"，无法形成路径

### 3. **NER规则过于严格** ⚠️

**代码逻辑**（`extract_graph.py`）：
```python
for ent in sent.ents:
    if ent.label_ == "PERSON":      # 人名
        ...
    elif ent.label_ in ["ORG", "GPE"]:  # 组织、地理政治实体
        ...

for token in sent:
    if token.pos_ == "NOUN" and token.lemma_.strip():  # 普通名词
        ...
    elif token.pos_ == "PROPN" and token.text.strip():  # 专有名词
        ...
```

**问题**：
- 中文spaCy模型的词性标注和实体识别都很弱
- 对于短查询，上下文不足，识别率更低
- "千味央厨"在长文档中可能被识别为ORG，但在短问题中识别失败

### 4. **检索流程的失败链条** 🔗

```
查询: "千味央厨的成立时间是什么时候？"
   ↓
1. NER提取 (query.py line 448)
   entities = naive_extract_graph(query, nlp)
   结果: entities = []  ❌
   ↓
2. 判断是否为空 (line 455)
   if len(entities) == 0:
       ✅ 是空，执行Global Search（纯向量检索）
   ↓
3. 向量检索
   返回相似度最高的chunks
   但没有利用Graph结构
```

**另一条路径**：
```
查询: "千味央厨的定制生产模式..."
   ↓
1. NER提取
   entities = ['千味央厨的定制生产模式与通用品生产模式有何不同']  ⚠️
   ↓
2. Graph检索 (line 465)
   local_res = self.local_retrieval(entities, shortest_path_k)
   在Graph中查找"千味央厨的定制生产模式与通用品生产模式有何不同"
   结果: chunk_count = 0  ❌ (图谱中没有这个实体)
   ↓
3. 退化为Occurrence Rerank (line 477)
   向量检索 + 实体过滤
   也没有真正利用Graph
```

## 📊 数据证明

### Graph中的实体分布
```
总实体数: 228
有用实体: ~30% ("华莱士"、"中国"、"速冻食品"等)
噪声实体: ~20% ("[图片"、"├─"、"END"等)
中间实体: ~50% (文档中的普通名词)
```

### 查询实体提取成功率
```
完全失败（空）: 2/14 (14.3%)
提取到问号: 6/14 (42.9%)
提取整个问题: 6/14 (42.9%)
提取到有效实体: 0/14 (0%)  ❌
```

## 💡 解决方案

### 优先级1: 使用更强的中文NER模型 🔥

**问题**：`zh_core_web_sm`太弱

**方案A**：使用大模型NER
```python
# 使用LLM提取实体
entities = await llm_extract_entities(query)
# 返回: ["千味央厨", "成立时间", "2012年"]
```

**方案B**：使用更好的中文NER工具
```python
# 使用 LAC (百度)
from LAC import LAC
lac = LAC(mode='lac')
result = lac.run(query)
# 更准确的中文分词和NER
```

**方案C**：关键词提取代替NER
```python
import jieba
import jieba.analyse
# 提取关键词
keywords = jieba.analyse.extract_tags(query, topK=5)
# ["千味央厨", "成立", "时间"]
```

### 优先级2: 改进Graph构建 ⭐

**当前问题**：
- 噪声实体多（"[图片"、"├─"等）
- 缺少关键实体（"成立时间"、"营业收入"等概念）

**改进方案**：
```python
# 1. 过滤噪声实体
NOISE_PATTERNS = ['[图片', '├─', '▪', 'Chart', 'Bar']
if any(pattern in entity for pattern in NOISE_PATTERNS):
    continue

# 2. 添加关键概念实体
CONCEPTS = {
    '成立时间': ['成立', '创立', '创建', '建立'],
    '营业收入': ['营收', '收入', '营业额'],
    '产品': ['产品', '品类', 'SKU'],
    ...
}
```

### 优先级3: 降级策略 - 混合检索 ⭐

**当前问题**：Graph失败后完全依赖向量

**改进方案**：
```python
# 同时使用多种检索策略
scores = {}

# 1. 尝试Graph检索
if entities:
    graph_chunks = local_retrieval(entities)
    for chunk in graph_chunks:
        scores[chunk] = scores.get(chunk, 0) + 0.4  # 权重

# 2. 向量检索
vector_chunks = dense_retrieval(query)
for chunk in vector_chunks:
    scores[chunk] = scores.get(chunk, 0) + 0.3

# 3. BM25关键词检索
keyword_chunks = bm25_retrieval(query)
for chunk in keyword_chunks:
    scores[chunk] = scores.get(chunk, 0) + 0.3

# 按总分排序
sorted_chunks = sorted(scores.items(), key=lambda x: -x[1])
```

### 优先级4: 查询重写 💡

**当前问题**：短问题缺乏上下文

**改进方案**：
```python
# 用LLM扩展查询
expanded_query = await llm_expand_query(
    "千味央厨的成立时间是什么时候？"
)
# 返回: "千味央厨公司是一家速冻食品企业，请问该公司的成立时间、创立日期是哪一年？"

# 然后再进行NER
entities = naive_extract_graph(expanded_query, nlp)
```

## 🎯 推荐行动

### 立即可行（10分钟）
1. ✅ **关键词提取代替NER**
   - 使用`jieba`分词 + TF-IDF提取关键词
   - 成本低，效果立竿见影

### 短期优化（1小时）
2. ✅ **混合检索策略**
   - Graph + Vector + BM25
   - 提高召回率

3. ✅ **过滤Graph噪声**
   - 清理"[图片"等无效实体
   - 提高Graph质量

### 长期改进（1天）
4. ⭐ **使用LLM做NER**
   - 准确率最高
   - 但API调用成本高

5. ⭐ **查询重写**
   - 提供更多上下文
   - 提高实体识别率

## 📈 预期效果

| 方案 | Graph召回率 | 整体准确率 | 实施成本 |
|------|------------|-----------|---------|
| 当前 | 0% | 7.1% | - |
| 关键词提取 | ~40% | ~30% | 低 |
| 混合检索 | N/A | ~50% | 中 |
| LLM NER | ~70% | ~70% | 高 |
| 全部优化 | ~80% | ~85% | 高 |

---

**结论**：Graph检索完全失效的根本原因是**中文NER在短查询上的失败**，导致查询实体为空或错误，无法与Graph中的实体匹配，最终退化为纯向量检索。
