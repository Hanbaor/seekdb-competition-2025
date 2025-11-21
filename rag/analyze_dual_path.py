#!/usr/bin/env python3
"""分析双路召回策略的效果"""

import json
import sys
from pathlib import Path
from collections import defaultdict


def analyze_dual_path(log_dir: str):
    """分析双路召回中假设答案路径和原问题路径的效果"""
    
    log_path = Path(log_dir)
    if not log_path.exists():
        # Try latest log
        logs_dir = Path(__file__).parent / 'logs'
        if logs_dir.exists():
            all_logs = sorted([d for d in logs_dir.iterdir() if d.is_dir()], reverse=True)
            if all_logs:
                log_path = all_logs[0]
                print(f"📁 使用最新运行: {log_path.name}\n")
            else:
                print("❌ 没有找到日志目录")
                return
        else:
            print(f"❌ 日志目录不存在: {log_dir}")
            return
    
    # Load ground truth
    gt_file = Path(__file__).parent / 'data' / 'answer.json'
    if not gt_file.exists():
        print(f"❌ Ground truth文件不存在: {gt_file}")
        return
    
    with open(gt_file, 'r', encoding='utf-8') as f:
        gt_data = json.load(f)
    
    ground_truth = {item['question']: item for item in gt_data}
    
    # Load detailed results
    detailed_file = log_path / 'detailed_results.json'
    if not detailed_file.exists():
        print(f"❌ 详细结果文件不存在: {detailed_file}")
        return
    
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    print("=" * 100)
    print("📊 双路召回策略效果分析")
    print("=" * 100)
    print()
    
    stats = {
        'total': 0,
        'correct': 0,
        'hypo_has_answer': 0,      # 假设答案路径召回了正确答案
        'question_has_answer': 0,   # 原问题路径召回了正确答案
        'both_have_answer': 0,      # 两个路径都召回了正确答案
        'only_hypo_has': 0,         # 只有假设答案路径召回
        'only_question_has': 0,     # 只有原问题路径召回
        'neither_has': 0,           # 两个路径都没召回
        'llm_picked_from_hypo': 0,  # LLM从假设答案路径中选择
        'llm_picked_from_question': 0,  # LLM从原问题路径中选择
    }
    
    details = []
    
    for idx, result in enumerate(results, 1):
        question = result.get('question', '')
        if question not in ground_truth:
            continue
        
        stats['total'] += 1
        
        gt = ground_truth[question]
        gt_filename = gt.get('filename', '')
        gt_page = gt.get('page', 0)
        
        pred_filename = result.get('filename', '')
        pred_page = result.get('page', 0)
        
        # Check if prediction is correct
        is_correct = (pred_filename == gt_filename and pred_page == gt_page)
        if is_correct:
            stats['correct'] += 1
        
        # Analyze retrieval results
        retrieval_results = result.get('retrieval_results', [])
        
        hypo_has = False
        question_has = False
        hypo_rank = None
        question_rank = None
        pred_from = None
        pred_source = None
        
        for ret in retrieval_results:
            ret_filename = ret.get('filename', '')
            ret_page = ret.get('page', 0)
            ret_rank = ret.get('rank', 0)
            ret_source = ret.get('source', 'unknown')
            
            # Check if this is the ground truth
            is_gt = (ret_filename == gt_filename and ret_page == gt_page)
            
            if is_gt:
                # Track which path(s) found the ground truth
                if ret_source == 'hypothetical':
                    hypo_has = True
                    if hypo_rank is None or ret_rank < hypo_rank:
                        hypo_rank = ret_rank
                elif ret_source == 'question':
                    question_has = True
                    if question_rank is None or ret_rank < question_rank:
                        question_rank = ret_rank
                else:
                    # Unknown source, count for both
                    hypo_has = True
                    question_has = True
            
            # Check if predicted result matches this retrieval
            if ret_filename == pred_filename and ret_page == pred_page:
                pred_from = ret_rank
                pred_source = ret_source
        
        # Update statistics based on which paths found the answer
        if hypo_has:
            stats['hypo_has_answer'] += 1
        if question_has:
            stats['question_has_answer'] += 1
        
        if hypo_has and question_has:
            stats['both_have_answer'] += 1
        elif hypo_has:
            stats['only_hypo_has'] += 1
        elif question_has:
            stats['only_question_has'] += 1
        else:
            stats['neither_has'] += 1
        
        # Track which path LLM selected from
        if pred_source == 'hypothetical':
            stats['llm_picked_from_hypo'] += 1
        elif pred_source == 'question':
            stats['llm_picked_from_question'] += 1
        
        details.append({
            'idx': idx,
            'question': question,
            'is_correct': is_correct,
            'hypo_has': hypo_has,
            'question_has': question_has,
            'hypo_rank': hypo_rank,
            'question_rank': question_rank,
            'pred_source': pred_source,
            'pred_filename': pred_filename,
            'pred_page': pred_page,
            'gt_filename': gt_filename,
            'gt_page': gt_page,
        })
    
    # Print summary statistics
    print(f"📈 整体统计 (共 {stats['total']} 个问题)")
    print(f"  ✅ 最终预测正确: {stats['correct']}/{stats['total']} ({stats['correct']/stats['total']*100:.1f}%)")
    print()
    
    print(f"🔍 双路召回效果对比:")
    print(f"  📊 假设答案路径召回正确: {stats['hypo_has_answer']}/{stats['total']} ({stats['hypo_has_answer']/stats['total']*100:.1f}%)")
    print(f"  📊 原问题路径召回正确: {stats['question_has_answer']}/{stats['total']} ({stats['question_has_answer']/stats['total']*100:.1f}%)")
    print()
    
    print(f"🔀 召回覆盖情况:")
    print(f"  ✅ 两条路径都召回: {stats['both_have_answer']}/{stats['total']} ({stats['both_have_answer']/stats['total']*100:.1f}%)")
    print(f"  🔵 仅假设答案召回: {stats['only_hypo_has']}/{stats['total']} ({stats['only_hypo_has']/stats['total']*100:.1f}%)")
    print(f"  🟢 仅原问题召回: {stats['only_question_has']}/{stats['total']} ({stats['only_question_has']/stats['total']*100:.1f}%)")
    print(f"  ❌ 都未召回: {stats['neither_has']}/{stats['total']} ({stats['neither_has']/stats['total']*100:.1f}%)")
    print()
    
    print(f"🎯 LLM Rerank选择来源:")
    print(f"  🔵 从假设答案路径选择: {stats['llm_picked_from_hypo']}/{stats['total']} ({stats['llm_picked_from_hypo']/stats['total']*100:.1f}%)")
    print(f"  🟢 从原问题路径选择: {stats['llm_picked_from_question']}/{stats['total']} ({stats['llm_picked_from_question']/stats['total']*100:.1f}%)")
    print()
    
    # Calculate effectiveness
    total_recall = stats['hypo_has_answer'] + stats['question_has_answer'] - stats['both_have_answer']
    single_best = max(stats['hypo_has_answer'], stats['question_has_answer'])
    improvement = total_recall - single_best
    
    print(f"💡 双路策略价值评估:")
    print(f"  单路最优召回: {single_best}/{stats['total']} ({single_best/stats['total']*100:.1f}%)")
    print(f"  双路合并召回: {total_recall}/{stats['total']} ({total_recall/stats['total']*100:.1f}%)")
    print(f"  提升效果: +{improvement} 题 (+{improvement/stats['total']*100:.1f}%)")
    print()
    
    if stats['only_hypo_has'] > 0:
        print(f"  ⭐ 假设答案路径独有贡献: {stats['only_hypo_has']} 题")
    if stats['only_question_has'] > 0:
        print(f"  ⭐ 原问题路径独有贡献: {stats['only_question_has']} 题")
    print()
    
    # Print detailed analysis
    print("=" * 100)
    print("📋 逐题详细分析:")
    print("=" * 100)
    
    for detail in details:
        status = "✅" if detail['is_correct'] else "❌"
        
        # Build recall status
        if detail['hypo_has'] and detail['question_has']:
            recall_icon = "✅✅"
            recall_text = f"两路都召回 (假设#{detail['hypo_rank']}, 原问#{detail['question_rank']})"
        elif detail['hypo_has']:
            recall_icon = "🔵"
            recall_text = f"仅假设答案召回 (排名#{detail['hypo_rank']})"
        elif detail['question_has']:
            recall_icon = "🟢"
            recall_text = f"仅原问题召回 (排名#{detail['question_rank']})"
        else:
            recall_icon = "❌"
            recall_text = "都未召回"
        
        # Build selection source
        if detail['pred_source'] == 'hypothetical':
            selection = "🔵 LLM从假设答案路径选择"
        elif detail['pred_source'] == 'question':
            selection = "🟢 LLM从原问题路径选择"
        else:
            selection = "⚪ 来源未知"
        
        print(f"\n{status} 问题 {detail['idx']}: {detail['question'][:60]}...")
        print(f"   预测: {detail['pred_filename'][:50]}... 第{detail['pred_page']}页")
        print(f"   正确: {detail['gt_filename'][:50]}... 第{detail['gt_page']}页")
        print(f"   {recall_icon} 召回: {recall_text}")
        print(f"   {selection}")
    
    print()
    print("=" * 100)


if __name__ == '__main__':
    log_dir = sys.argv[1] if len(sys.argv) > 1 else 'logs'
    analyze_dual_path(log_dir)
