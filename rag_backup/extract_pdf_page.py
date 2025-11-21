#!/usr/bin/env python3
"""
从PDF中提取指定页面为图片
用于准备VLM测试图片
"""

import sys
from pathlib import Path
from pdf2image import convert_from_path

def extract_page(pdf_path: str, page_num: int = 1, output_dir: str = "./test_images", dpi: int = 150):
    """
    从PDF中提取指定页面为图片
    
    Args:
        pdf_path: PDF文件路径
        page_num: 页码（从1开始）
        output_dir: 输出目录
        dpi: 图片分辨率
    """
    pdf_path = Path(pdf_path)
    
    # 检查PDF是否存在
    if not pdf_path.exists():
        print(f"❌ 错误: PDF文件不存在: {pdf_path}")
        return None
    
    print(f"\n{'='*60}")
    print(f"正在从PDF提取页面...")
    print(f"{'='*60}")
    print(f"PDF文件: {pdf_path}")
    print(f"提取页码: 第 {page_num} 页")
    print(f"输出目录: {output_dir}")
    print(f"分辨率: {dpi} DPI")
    print(f"{'='*60}\n")
    
    try:
        # 创建输出目录
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        # 转换指定页面
        print(f"⏳ 正在转换第 {page_num} 页...")
        images = convert_from_path(
            pdf_path,
            dpi=dpi,
            first_page=page_num,
            last_page=page_num
        )
        
        if not images:
            print(f"❌ 错误: 无法提取页面")
            return None
        
        # 保存图片
        image = images[0]
        output_file = output_path / f"{pdf_path.stem}_page_{page_num}.png"
        image.save(output_file, "PNG")
        
        print(f"✅ 页面提取成功！")
        print(f"   输出文件: {output_file}")
        print(f"   图片尺寸: {image.size[0]} x {image.size[1]} pixels")
        print(f"\n{'='*60}\n")
        
        return str(output_file)
        
    except Exception as e:
        print(f"❌ 提取失败: {str(e)}")
        return None

def extract_multiple_pages(pdf_path: str, pages: list, output_dir: str = "./test_images", dpi: int = 150):
    """提取多个页面"""
    results = []
    
    for page_num in pages:
        output_file = extract_page(pdf_path, page_num, output_dir, dpi)
        if output_file:
            results.append(output_file)
    
    print(f"✅ 完成！成功提取 {len(results)}/{len(pages)} 个页面")
    return results

def main():
    """主函数"""
    print("""
╔══════════════════════════════════════════════════════════════╗
║           PDF页面提取工具                                     ║
║           从PDF中提取指定页面为图片用于测试                   ║
╚══════════════════════════════════════════════════════════════╝
    """)
    
    # 检查命令行参数
    if len(sys.argv) < 2:
        print("使用方法:")
        print("  python extract_pdf_page.py <PDF路径> [页码1] [页码2] ...")
        print("\n示例:")
        print("  python extract_pdf_page.py ./document.pdf 1           # 提取第1页")
        print("  python extract_pdf_page.py ./document.pdf 1 3 5       # 提取第1、3、5页")
        print("  python extract_pdf_page.py ./document.pdf             # 提取第1页（默认）")
        return
    
    pdf_path = sys.argv[1]
    
    # 获取页码列表
    if len(sys.argv) > 2:
        pages = [int(p) for p in sys.argv[2:]]
    else:
        pages = [1]  # 默认第1页
    
    # 提取页面
    if len(pages) == 1:
        extract_page(pdf_path, pages[0])
    else:
        extract_multiple_pages(pdf_path, pages)

if __name__ == "__main__":
    main()
