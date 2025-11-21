import base64
import logging
from pathlib import Path
from typing import Dict, Any, Callable, List, Union

logger = logging.getLogger(__name__)

class VLMParser:
    def __init__(self, vision_model_func: Callable):
        self.vision_model_func = vision_model_func

    def _encode_image_to_base64(self, image_path: Union[str, Path]) -> str:
        with open(image_path, "rb") as image_file:
            return base64.b64encode(image_file.read()).decode('utf-8')

    async def analyze_page(self, image_path: Path, page_num: int) -> Dict[str, Any]:
        """
        Analyze a single page image using VLM.
        """
        # Encode image
        image_base64 = self._encode_image_to_base64(image_path)
        
        # Construct prompt (Copied from backup)
        system_prompt = """你是一个专业的文档分析助手。请仔细分析这个文档页面，并提取其中的所有内容。

要求：
1. 按照原文档的顺序和结构输出内容
2. 保留标题、段落、列表等格式结构
3. 对于表格，使用 Markdown 表格格式
4. 对于公式，使用 LaTeX 格式（用 $ 或 $$ 包围）
5. **对于图片/图表**：
   - 使用格式：`[图片描述: 详细描述图片内容，包括图表类型、数据趋势、关键信息等]`
   - 如果有图例或标注，也要包含在描述中
   - 例如：`[图片描述: 折线图显示2024年1-12月销售额从100万增长到300万，呈持续上升趋势]`
6. 整体输出为 Markdown 格式
7. 不要添加任何额外的解释、评论或分析，只输出文档的原始内容
8. 保持内容的完整性和准确性"""

        user_prompt = f"请提取并还原第 {page_num} 页的所有内容（文本、表格、公式、图片等）："
        
        # Build messages
        messages = [
            {"role": "system", "content": system_prompt},
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": user_prompt},
                    {
                        "type": "image_url",
                        "image_url": {
                            "url": f"data:image/png;base64,{image_base64}"
                        }
                    }
                ]
            }
        ]
        
        # Call VLM
        logger.info(f"[VLM] Analyzing page {page_num}...")
        try:
            result = await self.vision_model_func(messages=messages)
            logger.info(f"[VLM] Page {page_num} completed")
            
            return {
                "type": "text",
                "text": result,
                "page_idx": page_num - 1,
                "page_num": page_num,
            }
        except Exception as e:
            logger.error(f"[VLM] Failed page {page_num}: {e}")
            raise
