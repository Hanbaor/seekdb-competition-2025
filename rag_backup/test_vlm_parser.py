#!/usr/bin/env python3
"""
测试 VLM 解析器
用法：python test_vlm_parser.py <pdf_file_path>
"""

import asyncio
import sys
import os
from pathlib import Path

# 设置项目路径
PROJECT_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from dotenv import load_dotenv

# 加载环境变量
load_dotenv(dotenv_path=PROJECT_ROOT / ".env")

from src.config import load_pipeline_config
from src.anyrag_pipeline.builder import build_raganything


async def test_vlm_parser(pdf_path: str):
    """测试 VLM 解析器解析单个 PDF"""
    
    print("=" * 60)
    print("VLM 解析器测试")
    print("=" * 60)
    
    # 检查文件
    if not os.path.exists(pdf_path):
        print(f"❌ 错误：文件不存在 {pdf_path}")
        return
    
    print(f"\n📄 测试文件: {pdf_path}")
    
    # 加载配置
    print("\n⚙️  加载配置...")
    config = load_pipeline_config()
    
    # 检查是否使用 VLM 解析器
    if config.rag_config.parser != "vlm":
        print(f"⚠️  警告：当前配置使用的是 {config.rag_config.parser} 解析器")
        print("   请在 .env 文件中设置 PARSER=vlm")
        response = input("是否继续测试？(y/n): ")
        if response.lower() != 'y':
            return
    
    print(f"   解析器: {config.rag_config.parser}")
    print(f"   VLM 模型: {os.getenv('VLM_MODEL_NAME', 'N/A')}")
    print(f"   输出目录: {config.rag_config.parser_output_dir}")
    
    # 构建 RAG
    print("\n🔧 初始化 RAG 系统...")
    try:
        rag = build_raganything(config)
        print("   ✅ RAG 系统初始化成功")
    except Exception as e:
        print(f"   ❌ 初始化失败: {e}")
        return
    
    # 测试解析
    print(f"\n🚀 开始解析文档...")
    print("   （这可能需要几分钟，取决于 PDF 页数和 API 速度）")
    
    try:
        content_list, doc_id = await rag.parse_document(
            file_path=pdf_path,
            output_dir=config.rag_config.parser_output_dir
        )
        
        print(f"\n✅ 解析成功！")
        print(f"   文档 ID: {doc_id}")
        print(f"   提取的内容块数: {len(content_list)}")
        
        # 统计内容类型
        content_types = {}
        for item in content_list:
            if isinstance(item, dict):
                content_type = item.get("type", "unknown")
                content_types[content_type] = content_types.get(content_type, 0) + 1
        
        print(f"\n📊 内容统计:")
        for content_type, count in sorted(content_types.items()):
            print(f"   - {content_type}: {count}")
        
        # 显示前几个内容块的预览
        print(f"\n📝 内容预览（前 3 个块）:")
        for i, item in enumerate(content_list[:3], 1):
            if isinstance(item, dict):
                text = item.get("text", "")
                page_num = item.get("page_num", "?")
                preview = text[:100].replace("\n", " ")
                if len(text) > 100:
                    preview += "..."
                print(f"\n   块 {i} (页 {page_num}):")
                print(f"   {preview}")
        
        # 检查输出文件
        pdf_stem = Path(pdf_path).stem
        output_dir = Path(config.rag_config.parser_output_dir) / pdf_stem / "vlm"
        md_file = output_dir / f"{pdf_stem}.md"
        json_file = output_dir / f"{pdf_stem}_content_list.json"
        
        print(f"\n📁 输出文件:")
        if md_file.exists():
            print(f"   ✅ Markdown: {md_file}")
            print(f"      大小: {md_file.stat().st_size / 1024:.2f} KB")
        else:
            print(f"   ❌ Markdown 文件未找到")
        
        if json_file.exists():
            print(f"   ✅ JSON: {json_file}")
            print(f"      大小: {json_file.stat().st_size / 1024:.2f} KB")
        else:
            print(f"   ❌ JSON 文件未找到")
        
    except Exception as e:
        print(f"\n❌ 解析失败: {e}")
        import traceback
        traceback.print_exc()
        return
    
    print("\n" + "=" * 60)
    print("测试完成！")
    print("=" * 60)


async def test_simple_parse():
    """简单测试：不依赖完整 RAG 系统"""
    print("\n🧪 简单解析测试（仅测试 VLM Parser）")
    print("-" * 60)
    
    from src.anyrag_pipeline.raganything.parser import VLMParser
    from src.anyrag_pipeline.builder import _build_vision_model_func
    
    # 构建 vision model function
    api_key = os.getenv("OPENAI_API_KEY")
    base_url = os.getenv("OPENAI_BASE_URL")
    
    if not api_key:
        print("❌ 未找到 OPENAI_API_KEY，跳过简单测试")
        return
    
    vision_func = _build_vision_model_func(api_key, base_url)
    
    # 创建 VLM Parser
    parser = VLMParser(vision_model_func=vision_func)
    
    # 检查安装
    if not parser.check_installation():
        print("❌ pdf2image 未安装，请运行: pip install pdf2image")
        return
    
    print("✅ VLM Parser 初始化成功")
    print("✅ 依赖检查通过")


def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("用法: python test_vlm_parser.py <pdf_file_path>")
        print("\n示例:")
        print("  python test_vlm_parser.py ./data/dataset/sample.pdf")
        sys.exit(1)
    
    pdf_path = sys.argv[1]
    
    # 运行测试
    asyncio.run(test_vlm_parser(pdf_path))


if __name__ == "__main__":
    main()
