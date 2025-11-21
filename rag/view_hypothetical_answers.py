#!/usr/bin/env python3
"""
查看最新运行的假设答案
"""

import json
import os
from pathlib import Path

def view_hypothetical_answers(log_dir=None):
    """查看假设答案"""
    if not log_dir:
        # 找最新的log目录
        logs_dir = Path('logs')
        log_dirs = sorted([d for d in logs_dir.iterdir() if d.is_dir()], reverse=True)
        if not log_dirs:
            print("❌ 没有找到log目录")
            return
        log_dir = log_dirs[0]
    else:
        log_dir = Path(log_dir)
    
    detailed_file = log_dir / 'detailed_results.json'
    if not detailed_file.exists():
        print(f"❌ 文件不存在: {detailed_file}")
        return
    
    print(f"📁 读取: {log_dir.name}")
    print("=" * 100)
    print()
    
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    for idx, result in enumerate(results, 1):
        question = result.get('question', '')
        hypothetical = result.get('hypothetical_answer', '')
        filename = result.get('filename', '')
        page = result.get('page', 0)
        
        print(f"【问题 {idx}】")
        print(f"❓ 原问题: {question}")
        print(f"💡 假设答案: {hypothetical}")
        print(f"📄 选择文档: {filename.split('/')[-1] if filename else 'N/A'} 第{page}页")
        print("-" * 100)
        print()

if __name__ == "__main__":
    import sys
    log_dir = sys.argv[1] if len(sys.argv) > 1 else None
    view_hypothetical_answers(log_dir)
