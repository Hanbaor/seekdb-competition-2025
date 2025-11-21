# 调试输出说明

## 调试文件位置

运行 `main.py` 后，每个问题的详细调试信息会保存到：
```
debug_output/question_01.json
debug_output/question_02.json
...
debug_output/question_14.json
```

## 调试文件结构

每个 JSON 文件包含：

```json
{
  "question_id": 1,
  "question": "千味央厨的成立时间是什么时候？",
  "timestamp": "2025-11-21T14:00:00",
  
  "retrievals": [
    {
      "doc_name": "千味央厨-公司深度分析千滋百味厨师之选-22041226页",
      "doc_index": 0,
      "retrieval_type": "Local Search",  // 检索类型：Local/Global/Occurrence Rerank
      "entities": ["千味央厨", "成立时间"],  // 提取的实体
      "chunk_ids": {...},  // 检索到的 chunk IDs
      "chunks_text": "...",  // 检索到的完整文本
      "chunks_length": 5000,  // 文本长度
      "has_result": true  // 是否有结果
    },
    // ... 其他19个文档的检索结果
  ],
  
  "context": "...",  // 合并后的完整上下文
  "context_length": 50000,  // 上下文总长度
  "num_docs_retrieved": 3,  // 实际使用的文档数
  
  "prompt": "...",  // 发送给 LLM 的完整 prompt
  "prompt_length": 50200,
  
  "answer": "...",  // LLM 生成的答案
  
  "source": {
    "filename": "千味央厨-公司深度分析千滋百味厨师之选-22041226页.pdf",
    "page": 0
  }
}
```

## 使用分析脚本

### 1. 分析所有问题
```bash
python analyze_debug.py
```

输出示例：
```
📊 检索统计:
  检索文档数: 19
  成功返回结果: 5/19
  最终使用文档数: 3

🔍 各文档检索详情:
  [1] ✅ 千味央厨-公司深度分析千滋百味厨师之选-22041226页
      检索类型: Local Search
      提取实体: ['千味央厨', '成立时间']
      返回文本长度: 5432 字符
      内容预览: 千味央厨成立于2012年...

  [2] ❌ 千味央厨-深度报告深耕B端抢占蓝海-21090629页
      检索类型: Local Search
      提取实体: ['千味央厨', '成立时间']
      返回文本长度: 0 字符
```

### 2. 查看特定问题的完整上下文
```bash
python analyze_debug.py context 1
```

这会显示问题1的：
- 完整检索上下文
- 完整 LLM prompt

## 关键指标

### 检索类型
- **Local Search**: 图谱检索（实体+最短路径）
- **Global Search**: 纯向量检索（无实体）
- **Occurrence Rerank**: 向量检索+实体过滤

### 诊断问题

#### 问题1: 所有问题都检索到同一个文档
**症状**: 
```python
doc_usage = {
  "千味央厨-公司深度分析千滋百味厨师之选-22041226页": 14  # 所有14个问题
}
```

**可能原因**:
- max_chunk_setting=1 太严格
- 图谱构建问题（所有文档混在一起）
- 向量索引偏向某个文档

**解决方案**:
1. 增加 `max_chunk_setting` 到 3-5
2. 检查 `retrieval_type` 是否符合预期
3. 查看 `entities` 是否正确提取

#### 问题2: 检索成功率低
**症状**:
```
成功检索: 20/280 (7.1%)
```

**可能原因**:
- 实体提取失败（spaCy NER 问题）
- 图谱中没有对应的实体
- shortest_path_k 太严格

**解决方案**:
1. 查看 `entities` 字段，确认提取是否正确
2. 增加 `shortest_path_k`
3. 降低 `min_count`

#### 问题3: 上下文过长
**症状**:
```
context_length: 150000  # 超过 LLM 限制
```

**解决方案**:
1. 降低 `max_chunk_setting`
2. 在 prompt 生成前截断 context

## 快速诊断命令

```bash
# 查看所有问题的检索统计
python analyze_debug.py | grep "检索统计" -A 3

# 查看最常被检索到的文档
python analyze_debug.py | grep "最常被检索到" -A 10

# 查看某个具体问题的详情
python analyze_debug.py context 11  # 问题11是唯一文件名匹配的
```

## 与评测结果对比

评测得分 46.11 = 答案质量(44.5) + 文件名(1.8) + 页码(0)

可以用调试文件验证：
1. 查看 `doc_usage` 确认文件名匹配率
2. 查看 `source.page` 确认页码归因
3. 查看 `answer` vs 标准答案的相似度
