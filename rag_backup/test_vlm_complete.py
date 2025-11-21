#!/usr/bin/env python3
"""
VLM视觉解析完整测试
从PDF提取页面并进行VLM解析的一键测试脚本
"""

import asyncio
import sys
from pathlib import Path
from pdf2image import convert_from_path
from test_vlm_demo import analyze_image_with_vlm, print_result, save_result

def extract_pdf_page_to_temp(pdf_path: str, page_num: int = 1, dpi: int = 150):
    """
    从PDF提取页面到临时目录
    
    Args:
        pdf_path: PDF文件路径
        page_num: 页码
        dpi: 分辨率
    
    Returns:
        临时图片路径
    """
    pdf_path = Path(pdf_path)
    
    if not pdf_path.exists():
        raise FileNotFoundError(f"PDF文件不存在: {pdf_path}")
    
    print(f"📄 正在从PDF提取第 {page_num} 页...")
    
    # 创建临时目录
    temp_dir = Path("./temp_test_images")
    temp_dir.mkdir(parents=True, exist_ok=True)
    
    # 转换页面
    images = convert_from_path(
        pdf_path,
        dpi=dpi,
        first_page=page_num,
        last_page=page_num
    )
    
    if not images:
        raise RuntimeError("无法提取PDF页面")
    
    # 保存图片
    image = images[0]
    output_file = temp_dir / f"{pdf_path.stem}_page_{page_num}.png"
    image.save(output_file, "PNG")
    
    print(f"✅ 页面提取完成: {output_file}")
    print(f"   图片尺寸: {image.size[0]} x {image.size[1]} pixels\n")
    
    return str(output_file)

async def test_pdf_page(pdf_path: str, page_num: int = 1, save_output: bool = True):
    """
    测试PDF的指定页面
    
    Args:
        pdf_path: PDF文件路径
        page_num: 页码
        save_output: 是否保存输出
    """
    print(f"\n{'='*60}")
    print(f"VLM 视觉解析完整测试")
    print(f"{'='*60}")
    print(f"PDF文件: {pdf_path}")
    print(f"测试页码: 第 {page_num} 页")
    print(f"{'='*60}\n")
    
    try:
        # Step 1: 提取PDF页面
        image_path = extract_pdf_page_to_temp(pdf_path, page_num)
        
        # Step 2: 使用VLM分析
        print(f"🔍 开始VLM分析...\n")
        result = await analyze_image_with_vlm(image_path, page_num=page_num)
        
        # Step 3: 打印结果
        print_result(result)
        
        # Step 4: 保存结果
        if save_output:
            save_result(result)
        
        # 评估结果质量
        print(f"\n{'='*60}")
        print("📊 可行性评估指标:")
        print(f"{'='*60}")
        
        content = result['content']
        content_length = len(content)
        has_structure = any(marker in content for marker in ['#', '##', '|', '-', '*'])
        has_table = '|' in content and '-' in content
        has_formula = '$' in content
        has_image_desc = '[图片描述:' in content or '[图片描述：' in content
        
        print(f"✓ 内容长度: {content_length} 字符")
        print(f"✓ 结构化内容: {'是' if has_structure else '否'}")
        print(f"✓ 包含表格: {'是' if has_table else '否'}")
        print(f"✓ 包含公式: {'是' if has_formula else '否'}")
        print(f"✓ 包含图片描述: {'是' if has_image_desc else '否'}")
        print(f"✓ 处理耗时: {result['duration_seconds']:.2f} 秒")
        print(f"✓ Token消耗: {result['usage']['total_tokens']}")
        
        # 简单的可行性判断
        quality_score = 0
        if content_length > 100:
            quality_score += 1
        if has_structure:
            quality_score += 1
        if result['duration_seconds'] < 30:
            quality_score += 1
        
        print(f"\n{'='*60}")
        if quality_score >= 2:
            print("✅ 可行性评估: 良好 - VLM解析效果理想")
        elif quality_score == 1:
            print("⚠️  可行性评估: 一般 - VLM解析效果尚可，建议进一步优化")
        else:
            print("❌ 可行性评估: 较差 - VLM解析效果不佳")
        print(f"{'='*60}\n")
        
        return result
        
    except Exception as e:
        print(f"\n❌ 测试失败: {str(e)}")
        import traceback
        traceback.print_exc()
        return None

async def batch_test_pdf_pages(pdf_path: str, pages: list):
    """批量测试多个页面"""
    results = []
    
    for i, page_num in enumerate(pages, 1):
        print(f"\n{'#'*60}")
        print(f"# 测试进度: {i}/{len(pages)}")
        print(f"{'#'*60}\n")
        
        result = await test_pdf_page(pdf_path, page_num)
        if result:
            results.append(result)
        
        # 等待一下，避免API限流
        if i < len(pages):
            await asyncio.sleep(1)
    
    # 汇总统计
    print(f"\n{'='*60}")
    print(f"批量测试完成汇总")
    print(f"{'='*60}")
    print(f"测试页面数: {len(pages)}")
    print(f"成功解析数: {len(results)}")
    print(f"平均耗时: {sum(r['duration_seconds'] for r in results) / len(results):.2f} 秒")
    print(f"总Token消耗: {sum(r['usage']['total_tokens'] for r in results)}")
    print(f"{'='*60}\n")
    
    return results

async def main():
    """主函数"""
    print("""
╔══════════════════════════════════════════════════════════════╗
║           VLM 视觉解析完整测试工具                            ║
║           PDF提取 + VLM分析 + 可行性评估                      ║
╚══════════════════════════════════════════════════════════════╝
    """)
    
    if len(sys.argv) < 2:
        print("使用方法:")
        print("  python test_vlm_complete.py <PDF路径> [页码1] [页码2] ...")
        print("\n示例:")
        print("  python test_vlm_complete.py ./document.pdf              # 测试第1页")
        print("  python test_vlm_complete.py ./document.pdf 1            # 测试第1页")
        print("  python test_vlm_complete.py ./document.pdf 1 5 10       # 测试第1、5、10页")
        print("\n快速测试示例:")
        print("  python test_vlm_complete.py ./data/dataset/千味央厨-公司深度分析千滋百味厨师之选-22041226页.pdf 1")
        return
    
    pdf_path = sys.argv[1]
    
    # 获取页码
    if len(sys.argv) > 2:
        pages = [int(p) for p in sys.argv[2:]]
    else:
        pages = [1]
    
    # 执行测试
    if len(pages) == 1:
        await test_pdf_page(pdf_path, pages[0])
    else:
        await batch_test_pdf_pages(pdf_path, pages)

if __name__ == "__main__":
    asyncio.run(main())
