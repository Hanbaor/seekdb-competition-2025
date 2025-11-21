#!/usr/bin/env python3
"""
找出所有运行中准确率最高的那次
"""

import json
import os
from pathlib import Path

def check_accuracy(log_dir):
    """检查单次运行的准确率"""
    detailed_file = os.path.join(log_dir, 'detailed_results.json')
    if not os.path.exists(detailed_file):
        return None
    
    try:
        with open(detailed_file, 'r', encoding='utf-8') as f:
            results = json.load(f)
        
        with open('./data/answer.json', 'r', encoding='utf-8') as f:
            ground_truth = {item['question']: item for item in json.load(f)}
        
        correct = 0
        for r in results:
            q = r['question']
            if q not in ground_truth:
                continue
            gt = ground_truth[q]
            if r.get('filename') == gt['filename'] and r.get('page') == gt['page']:
                correct += 1
        
        return correct, len(results)
    except:
        return None

def main():
    logs_dir = Path('logs')
    all_runs = []
    
    for log_dir in sorted(logs_dir.iterdir(), reverse=True):
        if not log_dir.is_dir():
            continue
        
        result = check_accuracy(str(log_dir))
        if result:
            correct, total = result
            accuracy = correct / total * 100 if total > 0 else 0
            all_runs.append({
                'dir': log_dir.name,
                'correct': correct,
                'total': total,
                'accuracy': accuracy
            })
    
    # 按准确率排序
    all_runs.sort(key=lambda x: x['accuracy'], reverse=True)
    
    print("=" * 100)
    print("📊 所有运行结果（按准确率排序）")
    print("=" * 100)
    print()
    
    # 显示Top 20
    for i, run in enumerate(all_runs[:20], 1):
        marker = "🏆" if i <= 3 else "  "
        print(f"{marker} {i:2d}. {run['dir']}: {run['correct']}/{run['total']} = {run['accuracy']:5.1f}%")
    
    if len(all_runs) > 20:
        print(f"\n... 还有 {len(all_runs) - 20} 次运行")
    
    # 找出65分的可能运行
    print("\n" + "=" * 100)
    print("🔍 可能达到65分的运行（准确率 >= 40%）:")
    print("=" * 100)
    
    high_accuracy_runs = [r for r in all_runs if r['accuracy'] >= 40]
    if high_accuracy_runs:
        for run in high_accuracy_runs:
            print(f"✨ {run['dir']}: {run['correct']}/{run['total']} = {run['accuracy']:.1f}%")
    else:
        print("❌ 没有找到准确率 >= 40% 的运行")
        print(f"   最高准确率: {all_runs[0]['accuracy']:.1f}% ({all_runs[0]['dir']})")

if __name__ == "__main__":
    main()
