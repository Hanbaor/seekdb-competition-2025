# 解析器输出格式对比

## 问题背景

不同的解析器（MinerU, Docling, VLM Parser）输出的 JSON 格式有所不同。本文档说明各种格式及其兼容性。

## MinerU/Docling 标准格式

### 文本块
```json
{
  "type": "text",
  "text": "这是一段文本内容...",
  "page_idx": 0
}
```

### 图片块
```json
{
  "type": "image",
  "img_path": "/absolute/path/to/image.jpg",
  "image_caption": ["图片标题"],
  "image_footnote": ["图片注释"],
  "page_idx": 1
}
```

### 表格块
```json
{
  "type": "table",
  "img_path": "/path/to/table/image.jpg",  // 可选
  "table_body": "| 列1 | 列2 |\n|-----|-----|\n| 值1 | 值2 |",
  "table_caption": ["表格标题"],
  "table_footnote": ["表格注释"],
  "page_idx": 2
}
```

### 公式块
```json
{
  "type": "equation",
  "img_path": "/path/to/equation/image.jpg",
  "text": "E = mc^2",
  "text_format": "latex",
  "page_idx": 3
}
```

## VLM Parser 当前格式

### 统一文本格式
```json
{
  "type": "text",
  "text": "# 标题\n\n这是一段文本...\n\n| 列1 | 列2 |\n|-----|-----|\n| 值1 | 值2 |\n\n公式：$E=mc^2$",
  "page_idx": 0,
  "page_num": 1
}
```

**特点**：
- 所有内容统一为 Markdown 格式的文本
- 表格保留为 Markdown 表格
- 公式保留为 LaTeX 格式（`$...$` 或 `$$...$$`）
- 不单独提取图片、表格的结构化信息

## 格式兼容性分析

### separate_content 函数处理逻辑

```python
def separate_content(content_list):
    text_parts = []
    multimodal_items = []
    
    for item in content_list:
        content_type = item.get("type", "text")
        
        if content_type == "text":
            # 提取文本
            text = item.get("text", "")
            if text.strip():
                text_parts.append(text)
        elif content_type == "discarded":
            # 跳过丢弃内容
            continue
        else:
            # 多模态内容
            multimodal_items.append(item)
    
    return "\n\n".join(text_parts), multimodal_items
```

### VLM Parser 兼容性

| 场景 | 配置 | 兼容性 | 说明 |
|------|------|--------|------|
| **纯文本RAG** | 多模态处理关闭 | ✅ 完全兼容 | 所有内容作为文本处理，Markdown格式保留表格和公式 |
| **多模态RAG** | 多模态处理开启 | ⚠️ 部分兼容 | 图片、表格没有单独的结构化信息，无法进行专门处理 |

## 推荐配置

### 场景1：比赛服务器（无GPU，需避免超时）

**推荐使用 VLM Parser + 关闭多模态处理**

```bash
# .env 配置
PARSER=vlm
ENABLE_IMAGE_PROCESSING=false
ENABLE_TABLE_PROCESSING=false
ENABLE_EQUATION_PROCESSING=false
```

**原因**：
- ✅ 快速，不超时
- ✅ 表格和公式仍包含在文本中（Markdown格式）
- ✅ 对大多数RAG应用足够
- ✅ 格式完全兼容

### 场景2：本地开发（有GPU，追求最佳质量）

**推荐使用 MinerU + 开启多模态处理**

```bash
# .env 配置
PARSER=mineru
ENABLE_IMAGE_PROCESSING=true
ENABLE_TABLE_PROCESSING=true
ENABLE_EQUATION_PROCESSING=true
```

**原因**：
- ✅ 结构化提取图片、表格
- ✅ 支持专门的多模态处理
- ✅ 最佳质量

## 实际效果对比

### 示例：包含表格的页面

**MinerU 输出**（2个块）：
```json
[
  {
    "type": "text",
    "text": "下面是一个数据表格：",
    "page_idx": 0
  },
  {
    "type": "table",
    "table_body": "| 项目 | 数值 |\n|------|------|\n| A | 100 |\n| B | 200 |",
    "table_caption": ["数据统计表"],
    "page_idx": 0
  }
]
```

**VLM Parser 输出**（1个块）：
```json
[
  {
    "type": "text",
    "text": "下面是一个数据表格：\n\n| 项目 | 数值 |\n|------|------|\n| A | 100 |\n| B | 200 |",
    "page_idx": 0
  }
]
```

### 处理结果

**多模态关闭时**：
- 两种格式都会被合并为纯文本
- 效果相同

**多模态开启时**：
- MinerU：表格会被专门处理（如生成表格描述、提取关键信息等）
- VLM Parser：表格作为普通文本处理

## 结论

✅ **VLM Parser 的格式在推荐配置下是完全符合要求的**

- 当前 `.env` 配置已关闭多模态处理
- VLM Parser 输出的 `{"type": "text", "text": "...", "page_idx": 0}` 格式
- 与 `separate_content` 函数完全兼容
- 适合大多数 RAG 应用场景

⚠️ **如果需要完整的多模态支持**

- 建议在有 GPU 的环境使用 MinerU/Docling
- 或者进一步改进 VLM Parser，让它识别并分离不同类型的内容

📌 **最佳实践**

比赛服务器环境：`PARSER=vlm` + 多模态关闭 → **快速、不超时、格式兼容**
