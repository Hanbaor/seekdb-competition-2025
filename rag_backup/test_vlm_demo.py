#!/usr/bin/env python3
"""
VLM视觉解析测试Demo
用于验证视觉解析的可行性，给定图片返回解析结果
"""

import asyncio
import json
import base64
import os
import sys
from pathlib import Path
from datetime import datetime
from dotenv import load_dotenv

# 加载环境变量
load_dotenv()

# 添加src路径以导入lightrag
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'src'))
from lightrag.llm.openai import openai_complete_if_cache

def encode_image_to_base64(image_path: str) -> str:
    """将图片编码为base64字符串"""
    with open(image_path, "rb") as f:
        return base64.b64encode(f.read()).decode("utf-8")

async def analyze_image_with_vlm(image_path: str, page_num: int = 1, vlm_model: str = None) -> dict:
    """
    使用VLM分析图片内容（与raganything使用完全相同的提示词和格式）
    
    Args:
        image_path: 图片路径
        page_num: 页码（用于提示词）
        vlm_model: VLM模型名称
    
    Returns:
        解析结果字典
    """
    # 从环境变量获取VLM模型
    if vlm_model is None:
        vlm_model = os.getenv("VLM_MODEL_NAME", "qwen3-vl-plus")
    
    print(f"\n{'='*60}")
    print(f"正在分析图片: {image_path}")
    print(f"使用模型: {vlm_model}")
    print(f"{'='*60}\n")
    
    # 编码图片
    image_base64 = encode_image_to_base64(image_path)
    
    # 构建VLM提示词（与实际解析器使用的相同）
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
    
    # 构建消息
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
    
    # 调用VLM（使用与实际代码相同的方式）
    start_time = datetime.now()
    try:
        api_key = os.getenv("OPENAI_API_KEY")
        base_url = os.getenv("OPENAI_BASE_URL")
        
        content = await openai_complete_if_cache(
            model=vlm_model,
            prompt="",  # 空prompt，因为我们使用messages
            system_prompt=None,
            history_messages=[],
            messages=messages,
            api_key=api_key,
            base_url=base_url,
            temperature=0.1,
            max_tokens=4096
        )
        
        end_time = datetime.now()
        duration = (end_time - start_time).total_seconds()
        
        # 提取结果（包含raganything格式的content_entry）
        content_entry = {
            "type": "text",
            "text": content,
            "page_idx": page_num - 1,  # 0-indexed
            "page_num": page_num,  # 1-indexed
        }
        
        result = {
            "image_path": str(image_path),
            "model": vlm_model,
            "duration_seconds": duration,
            "timestamp": datetime.now().isoformat(),
            "content": content,
            "content_entry": content_entry,  # raganything格式
            "usage": {
                "prompt_tokens": "N/A",
                "completion_tokens": "N/A",
                "total_tokens": len(content)
            }
        }
        
        return result
        
    except Exception as e:
        print(f"❌ 分析失败: {str(e)}")
        raise

def print_result(result: dict):
    """打印结果"""
    print(f"\n{'='*60}")
    print("✅ 分析完成！")
    print(f"{'='*60}")
    print(f"图片路径: {result['image_path']}")
    print(f"使用模型: {result['model']}")
    print(f"耗时: {result['duration_seconds']:.2f} 秒")
    print(f"Token使用: {result['usage']['total_tokens']} (输入: {result['usage']['prompt_tokens']}, 输出: {result['usage']['completion_tokens']})")
    print(f"\n{'='*60}")
    print("解析内容:")
    print(f"{'='*60}\n")
    print(result['content'])
    print(f"\n{'='*60}")
    print("RAGAnything格式 (content_entry):")
    print(f"{'='*60}")
    import json
    print(json.dumps(result['content_entry'], ensure_ascii=False, indent=2))
    print(f"{'='*60}\n")

def save_result(result: dict, output_dir: str = "./vlm_test_output"):
    """保存结果到文件"""
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    # 生成文件名
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    image_name = Path(result['image_path']).stem
    
    # 保存JSON
    json_file = output_path / f"{image_name}_{timestamp}.json"
    with open(json_file, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    
    # 保存Markdown
    md_file = output_path / f"{image_name}_{timestamp}.md"
    with open(md_file, "w", encoding="utf-8") as f:
        f.write(f"# VLM解析结果\n\n")
        f.write(f"- **图片**: {result['image_path']}\n")
        f.write(f"- **模型**: {result['model']}\n")
        f.write(f"- **时间**: {result['timestamp']}\n")
        f.write(f"- **耗时**: {result['duration_seconds']:.2f} 秒\n")
        f.write(f"- **Token**: {result['usage']['total_tokens']}\n")
        f.write(f"\n---\n\n")
        f.write(result['content'])
    
    print(f"📁 结果已保存:")
    print(f"   JSON: {json_file}")
    print(f"   Markdown: {md_file}")

async def test_single_image(image_path: str, save_output: bool = True):
    """测试单张图片"""
    # 检查图片是否存在
    if not Path(image_path).exists():
        print(f"❌ 错误: 图片不存在: {image_path}")
        return None
    
    # 分析图片
    result = await analyze_image_with_vlm(image_path)
    
    # 打印结果
    print_result(result)
    
    # 保存结果
    if save_output:
        save_result(result)
    
    return result

async def test_multiple_images(image_paths: list, save_output: bool = True):
    """测试多张图片"""
    results = []
    
    for i, image_path in enumerate(image_paths, 1):
        print(f"\n📸 测试图片 {i}/{len(image_paths)}")
        try:
            result = await test_single_image(image_path, save_output)
            if result:
                results.append(result)
        except Exception as e:
            print(f"❌ 处理失败: {image_path}")
            print(f"   错误: {str(e)}")
    
    print(f"\n{'='*60}")
    print(f"✅ 测试完成！成功处理 {len(results)}/{len(image_paths)} 张图片")
    print(f"{'='*60}\n")
    
    return results

async def main():
    """主函数"""
    import sys
    
    print("""
╔══════════════════════════════════════════════════════════════╗
║           VLM 视觉解析测试 Demo                              ║
║           用于验证视觉解析的可行性                            ║
╚══════════════════════════════════════════════════════════════╝
    """)
    
    # 检查命令行参数
    if len(sys.argv) < 2:
        print("使用方法:")
        print("  python test_vlm_demo.py <图片路径1> [图片路径2] [图片路径3] ...")
        print("\n示例:")
        print("  python test_vlm_demo.py ./test_image.png")
        print("  python test_vlm_demo.py ./image1.png ./image2.jpg ./image3.png")
        print("\n提示: 可以使用PDF转换的图片进行测试")
        return
    
    # 获取图片路径
    image_paths = sys.argv[1:]
    
    # 测试图片
    if len(image_paths) == 1:
        await test_single_image(image_paths[0])
    else:
        await test_multiple_images(image_paths)

if __name__ == "__main__":
    asyncio.run(main())
