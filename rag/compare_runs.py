#!/usr/bin/env python3
"""
对比两次运行结果，找出分数变化的原因
"""

import json
import sys

def load_results(log_dir):
    """加载详细结果"""
    with open(f'{log_dir}/detailed_results.json', 'r', encoding='utf-8') as f:
        return json.load(f)

def compare_runs(run1_dir, run2_dir):
    """对比两次运行"""
    results1 = load_results(run1_dir)
    results2 = load_results(run2_dir)
    
    # 加载ground truth
    with open('./data/answer.json', 'r', encoding='utf-8') as f:
        ground_truth = {item['question']: item for item in json.load(f)}
    
    print("=" * 100)
    print(f"📊 对比运行结果")
    print("=" * 100)
    print(f"Run 1: {run1_dir}")
    print(f"Run 2: {run2_dir}")
    print()
    
    # 按问题对比
    differences = []
    
    for i, (item1, item2) in enumerate(zip(results1, results2), 1):
        q = item1['question']
        if q not in ground_truth:
            continue
        
        gt = ground_truth[q]
        
        # Run 1
        file1 = item1.get('filename', '')
        page1 = item1.get('page', 0)
        file1_match = (file1 == gt['filename'])
        page1_match = (page1 == gt['page'])
        
        # Run 2
        file2 = item2.get('filename', '')
        page2 = item2.get('page', 0)
        file2_match = (file2 == gt['filename'])
        page2_match = (page2 == gt['page'])
        
        # 检查是否有变化
        if file1 != file2 or page1 != page2:
            differences.append({
                'question': q,
                'gt_file': gt['filename'].split('/')[-1],
                'gt_page': gt['page'],
                'run1_file': file1.split('/')[-1] if file1 else 'N/A',
                'run1_page': page1,
                'run1_correct': file1_match and page1_match,
                'run2_file': file2.split('/')[-1] if file2 else 'N/A',
                'run2_page': page2,
                'run2_correct': file2_match and page2_match,
            })
    
    print(f"\n🔍 发现 {len(differences)} 个问题的结果发生了变化:\n")
    
    if not differences:
        print("✅ 两次运行结果完全一致")
        return
    
    for i, diff in enumerate(differences, 1):
        print(f"{i}. {diff['question'][:70]}...")
        print(f"   正确答案: {diff['gt_file']} 第{diff['gt_page']}页")
        print(f"   Run 1: {diff['run1_file']} 第{diff['run1_page']}页 {'✅' if diff['run1_correct'] else '❌'}")
        print(f"   Run 2: {diff['run2_file']} 第{diff['run2_page']}页 {'✅' if diff['run2_correct'] else '❌'}")
        
        if diff['run1_correct'] and not diff['run2_correct']:
            print("   ⚠️  Run 2 退步了！")
        elif not diff['run1_correct'] and diff['run2_correct']:
            print("   ✨ Run 2 改进了！")
        elif not diff['run1_correct'] and not diff['run2_correct']:
            print("   ⚠️  两次都错了，但错法不同")
        print()
    
    # 统计
    run1_correct = sum(1 for d in differences if d['run1_correct'])
    run2_correct = sum(1 for d in differences if d['run2_correct'])
    
    print("=" * 100)
    print("📈 变化统计:")
    print(f"  在{len(differences)}个变化的问题中:")
    print(f"  Run 1 正确: {run1_correct}/{len(differences)}")
    print(f"  Run 2 正确: {run2_correct}/{len(differences)}")
    
    if run1_correct > run2_correct:
        print(f"\n  ⚠️  Run 2 在变化的问题上表现更差 (少对了{run1_correct - run2_correct}个)")
    elif run2_correct > run1_correct:
        print(f"\n  ✨ Run 2 在变化的问题上表现更好 (多对了{run2_correct - run1_correct}个)")
    else:
        print(f"\n  ➡️  两次在变化的问题上正确数相同，但答案不同（可能是LLM随机性）")
    
    # 整体准确率
    print("\n" + "=" * 100)
    print("📊 整体准确率:")
    
    total = len(results1)
    run1_total_correct = 0
    run2_total_correct = 0
    
    for item1, item2 in zip(results1, results2):
        q = item1['question']
        if q not in ground_truth:
            continue
        gt = ground_truth[q]
        
        if item1.get('filename') == gt['filename'] and item1.get('page') == gt['page']:
            run1_total_correct += 1
        if item2.get('filename') == gt['filename'] and item2.get('page') == gt['page']:
            run2_total_correct += 1
    
    print(f"  Run 1: {run1_total_correct}/{total} = {run1_total_correct/total*100:.1f}%")
    print(f"  Run 2: {run2_total_correct}/{total} = {run2_total_correct/total*100:.1f}%")
    
    diff_count = run1_total_correct - run2_total_correct
    if diff_count > 0:
        print(f"\n  ⚠️  Run 2 退步了 {diff_count} 个问题")
    elif diff_count < 0:
        print(f"\n  ✨ Run 2 改进了 {-diff_count} 个问题")
    else:
        print(f"\n  ➡️  整体正确数相同")

if __name__ == "__main__":
    if len(sys.argv) == 3:
        compare_runs(sys.argv[1], sys.argv[2])
    else:
        # 默认对比最近两次
        compare_runs('logs/20251122_021911', 'logs/20251122_022532')
