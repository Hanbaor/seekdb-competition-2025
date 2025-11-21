#!/usr/bin/env python3
"""
对比假设性答案和正确答案（ground truth）
"""

import json
import os
from pathlib import Path

def compare_hypothetical_vs_truth(log_dir=None):
    """对比假设答案和真实答案"""
    # Load ground truth
    with open('./data/answer.json', 'r', encoding='utf-8') as f:
        ground_truth = {item['question']: item for item in json.load(f)}
    
    # Load latest run
    if not log_dir:
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
    
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    print(f"📁 分析运行: {log_dir.name}")
    print("=" * 120)
    print()
    
    # Statistics
    stats = {
        'total': 0,
        'both_correct': 0,  # 假设答案和真实答案都正确
        'hypo_better': 0,   # 假设答案包含更多关键信息
        'truth_better': 0,  # 真实答案更准确
        'both_incomplete': 0  # 两者都不够完整
    }
    
    for idx, result in enumerate(results, 1):
        question = result.get('question', '')
        hypothetical = result.get('hypothetical_answer', '')
        
        if question not in ground_truth:
            continue
        
        gt = ground_truth[question]
        gt_answer = gt.get('answer', '')
        gt_filename = gt.get('filename', '')
        gt_page = gt.get('page', 0)
        
        pred_filename = result.get('filename', '')
        pred_page = result.get('page', 0)
        
        # Check if prediction is correct
        file_match = (pred_filename == gt_filename)
        page_match = (pred_page == gt_page)
        both_match = file_match and page_match
        
        stats['total'] += 1
        
        print(f"{'='*120}")
        print(f"【问题 {idx}】")
        print(f"❓ 问题: {question}")
        print(f"\n📖 正确答案(Ground Truth):")
        print(f"   {gt_answer}")
        print(f"   📄 来源: {gt_filename.split('/')[-1]} 第{gt_page}页")
        print(f"\n💡 假设答案(Hypothetical):")
        print(f"   {hypothetical}")
        
        # Analysis
        print(f"\n🔍 分析:")
        
        # Check if hypothetical answer contains key information
        gt_lower = gt_answer.lower()
        hypo_lower = hypothetical.lower()
        
        # Extract key numbers/keywords from ground truth
        import re
        gt_numbers = set(re.findall(r'\d+(?:\.\d+)?%?', gt_answer))
        hypo_numbers = set(re.findall(r'\d+(?:\.\d+)?%?', hypothetical))
        
        number_overlap = gt_numbers & hypo_numbers
        
        print(f"   - 数字信息重叠: {len(number_overlap)}/{len(gt_numbers)} ({number_overlap if number_overlap else '无'})")
        
        # Check if hypothetical provides context that ground truth doesn't
        hypo_words = set(hypo_lower.split())
        gt_words = set(gt_lower.split())
        extra_context = hypo_words - gt_words
        
        print(f"   - 假设答案额外提供的词汇数: {len(extra_context)}")
        
        # Prediction accuracy
        if both_match:
            print(f"   ✅ 预测结果: 正确 ({pred_filename.split('/')[-1]} 第{pred_page}页)")
            if number_overlap:
                stats['both_correct'] += 1
                print(f"   ⭐ 假设答案包含正确信息，帮助准确检索")
        else:
            print(f"   ❌ 预测结果: 错误")
            print(f"      预测: {pred_filename.split('/')[-1]} 第{pred_page}页")
            print(f"      正确: {gt_filename.split('/')[-1]} 第{gt_page}页")
            
            if number_overlap:
                print(f"   ⚠️  假设答案虽包含正确信息，但检索仍失败")
            else:
                print(f"   ⚠️  假设答案缺少关键信息")
        
        # Length comparison
        print(f"\n📏 长度对比:")
        print(f"   - 正确答案: {len(gt_answer)} 字符")
        print(f"   - 假设答案: {len(hypothetical)} 字符")
        print(f"   - 原问题: {len(question)} 字符")
        
        print()
    
    # Summary
    print("=" * 120)
    print("📊 总体统计:")
    print("=" * 120)
    print(f"总问题数: {stats['total']}")
    print(f"假设答案帮助正确检索: {stats['both_correct']}/{stats['total']} ({stats['both_correct']/stats['total']*100:.1f}%)")
    print()

if __name__ == "__main__":
    import sys
    log_dir = sys.argv[1] if len(sys.argv) > 1 else None
    compare_hypothetical_vs_truth(log_dir)
