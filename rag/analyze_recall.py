#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
分析Top30召回结果中是否包含正确的文件名和页码
"""

import json
from typing import Dict, List, Tuple


def analyze_recall(top_k=10):
    """分析每个问题的TopK召回情况"""
    
    # 加载数据 - 使用最新的结果
    with open('logs/20251121_235733/detailed_results.json', 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    with open('data/answer.json', 'r', encoding='utf-8') as f:
        answers = json.load(f)
    
    # 构建标准答案字典
    ground_truth = {ans['question']: ans for ans in answers}
    
    print("=" * 100)
    print(f"Top{top_k}召回质量分析")
    print("=" * 100)
    
    # 统计变量
    total_questions = 0
    filename_in_top30 = 0
    both_in_top30 = 0
    
    filename_ranks = []  # 正确文件名的排名
    both_ranks = []      # 文件名+页码都正确的排名
    
    print(f"\n{'问题':<50} {'文件召回':<15} {'完全召回':<15} {'排名'}")
    print("-" * 100)
    
    for item in results:
        question = item['question']
        if question not in ground_truth:
            continue
        
        total_questions += 1
        gt = ground_truth[question]
        gt_filename = gt['filename']
        gt_page = gt['page']
        
        retrieval_results = item.get('retrieval_results', [])
        
        # 检查TopK中是否有正确答案
        file_found_rank = None
        both_found_rank = None
        
        for idx, r in enumerate(retrieval_results[:top_k], 1):
            r_filename = r.get('filename', '')
            r_page = r.get('page', 0)
            
            # 检查文件名
            if r_filename == gt_filename and file_found_rank is None:
                file_found_rank = idx
                filename_in_top30 += 1
                filename_ranks.append(idx)
                
                # 检查是否页码也对
                if r_page == gt_page:
                    both_found_rank = idx
                    both_in_top30 += 1
                    both_ranks.append(idx)
        
        # 继续检查页码（如果文件名已找到但页码还没对）
        if file_found_rank and not both_found_rank:
            for idx, r in enumerate(retrieval_results[:top_k], 1):
                if r.get('filename') == gt_filename and r.get('page') == gt_page:
                    both_found_rank = idx
                    both_in_top30 += 1
                    both_ranks.append(idx)
                    break
        
        # 打印结果
        q_short = question[:45] + "..." if len(question) > 45 else question
        file_status = f"✅ Rank{file_found_rank}" if file_found_rank else "❌ 未召回"
        both_status = f"✅ Rank{both_found_rank}" if both_found_rank else "❌ 未召回"
        
        rank_info = ""
        if both_found_rank:
            rank_info = f"文件+页码: Rank{both_found_rank}"
        elif file_found_rank:
            rank_info = f"仅文件: Rank{file_found_rank}"
        
        print(f"{q_short:<50} {file_status:<15} {both_status:<15} {rank_info}")
    
    print("\n" + "=" * 100)
    print("召回统计总结")
    print("=" * 100)
    
    # 计算召回率
    file_recall = filename_in_top30 / total_questions * 100 if total_questions > 0 else 0
    both_recall = both_in_top30 / total_questions * 100 if total_questions > 0 else 0
    
    print(f"\n总问题数: {total_questions}")
    print(f"\n✅ 文件名在Top{top_k}中: {filename_in_top30}/{total_questions} ({file_recall:.2f}%)")
    print(f"✅ 文件名+页码都在Top{top_k}中: {both_in_top30}/{total_questions} ({both_recall:.2f}%)")
    print(f"❌ 文件名都不在Top{top_k}: {total_questions - filename_in_top30}/{total_questions} ({100-file_recall:.2f}%)")
    
    # 排名分布统计
    if filename_ranks:
        print(f"\n📊 正确文件名排名分布:")
        print(f"   平均排名: {sum(filename_ranks) / len(filename_ranks):.1f}")
        print(f"   最好排名: Rank{min(filename_ranks)}")
        print(f"   最差排名: Rank{max(filename_ranks)}")
        
        # 按区间统计
        top1 = sum(1 for r in filename_ranks if r == 1)
        top3 = sum(1 for r in filename_ranks if r <= 3)
        top5 = sum(1 for r in filename_ranks if r <= 5)
        top10 = sum(1 for r in filename_ranks if r <= 10)
        
        print(f"   Rank1: {top1} ({top1/total_questions*100:.1f}%)")
        print(f"   Top3内: {top3} ({top3/total_questions*100:.1f}%)")
        print(f"   Top5内: {top5} ({top5/total_questions*100:.1f}%)")
        print(f"   Top10内: {top10} ({top10/total_questions*100:.1f}%)")
    
    if both_ranks:
        print(f"\n📊 完全正确(文件名+页码)排名分布:")
        print(f"   平均排名: {sum(both_ranks) / len(both_ranks):.1f}")
        print(f"   最好排名: Rank{min(both_ranks)}")
        print(f"   最差排名: Rank{max(both_ranks)}")
        
        # 按区间统计
        top1 = sum(1 for r in both_ranks if r == 1)
        top3 = sum(1 for r in both_ranks if r <= 3)
        top5 = sum(1 for r in both_ranks if r <= 5)
        top10 = sum(1 for r in both_ranks if r <= 10)
        
        print(f"   Rank1: {top1} ({top1/total_questions*100:.1f}%)")
        print(f"   Top3内: {top3} ({top3/total_questions*100:.1f}%)")
        print(f"   Top5内: {top5} ({top5/total_questions*100:.1f}%)")
        print(f"   Top10内: {top10} ({top10/total_questions*100:.1f}%)")
    
    # 详细分析未召回的问题
    print("\n" + "=" * 100)
    print(f"❌ 未召回分析 (文件名都不在Top{top_k})")
    print("=" * 100)
    
    for item in results:
        question = item['question']
        if question not in ground_truth:
            continue
        
        gt = ground_truth[question]
        gt_filename = gt['filename']
        retrieval_results = item.get('retrieval_results', [])
        
        # 检查是否召回
        found = any(r.get('filename') == gt_filename for r in retrieval_results[:top_k])
        
        if not found:
            print(f"\nQ: {question}")
            print(f"   正确文件: {gt_filename}")
            print(f"   正确页码: {gt['page']}")
            print(f"   Top3召回:")
            for i, r in enumerate(retrieval_results[:3], 1):
                print(f"      {i}. {r.get('filename', 'Unknown')[:60]} 第{r.get('page', 0)}页 (分数: {r.get('score', 0):.3f})")
    
    print("\n" + "=" * 100)


if __name__ == "__main__":
    import sys
    top_k = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    print(f"\n分析参数: Top{top_k}召回\n")
    analyze_recall(top_k)
