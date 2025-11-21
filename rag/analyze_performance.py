#!/usr/bin/env python3
"""
全面分析RAG系统性能
- 回答质量分析
- 文档选择准确率
- 页码选择准确率
- Top50召回率
"""

import json
import os
from collections import defaultdict
from typing import Dict, List

def load_data():
    """加载评估数据和ground truth"""
    # Load ground truth
    with open('./data/answer.json', 'r', encoding='utf-8') as f:
        train_data = json.load(f)
    ground_truth = {item['question']: item for item in train_data}
    
    # Load latest results
    log_dir = find_latest_log()
    if not log_dir:
        print("❌ 找不到日志目录")
        return None, None
    
    detailed_file = os.path.join(log_dir, 'detailed_results.json')
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    return results, ground_truth

def find_latest_log():
    """查找最新的日志目录"""
    logs_dir = "logs"
    if not os.path.exists(logs_dir):
        return None
    
    subdirs = [d for d in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, d))]
    if not subdirs:
        return None
    
    subdirs.sort(reverse=True)
    return os.path.join(logs_dir, subdirs[0])

def analyze_answer_scores(results: List[Dict], ground_truth: Dict):
    """分析回答得分情况"""
    print("\n" + "="*80)
    print("📊 1. 回答质量分析")
    print("="*80)
    
    score_ranges = {
        '优秀(90-100)': 0,
        '良好(80-89)': 0,
        '中等(70-79)': 0,
        '及格(60-69)': 0,
        '不及格(<60)': 0
    }
    
    scores = []
    for item in results:
        q = item['question']
        if q not in ground_truth:
            continue
        
        # 这里需要调用eval.py的评分逻辑，简化起见，我们分析retrieval_results
        # 实际应该从eval.py的输出获取
        pass
    
    print("注：需要运行eval.py获取详细得分")

def analyze_document_accuracy(results: List[Dict], ground_truth: Dict):
    """分析文档选择准确率"""
    print("\n" + "="*80)
    print("📁 2. 文档选择准确率")
    print("="*80)
    
    total = 0
    correct_file = 0
    correct_both = 0
    
    error_cases = []
    
    for item in results:
        q = item['question']
        if q not in ground_truth:
            continue
        
        total += 1
        gt = ground_truth[q]
        
        pred_file = item.get('filename', '')
        pred_page = item.get('page', 0)
        gt_file = gt['filename']
        gt_page = gt['page']
        
        file_match = (pred_file == gt_file)
        both_match = (file_match and pred_page == gt_page)
        
        if file_match:
            correct_file += 1
        if both_match:
            correct_both += 1
        else:
            error_cases.append({
                'question': q[:60] + '...',
                'pred_file': pred_file.split('/')[-1] if pred_file else 'N/A',
                'pred_page': pred_page,
                'gt_file': gt_file.split('/')[-1],
                'gt_page': gt_page,
                'file_match': file_match
            })
    
    print(f"\n总问题数: {total}")
    print(f"文档选择正确: {correct_file}/{total} = {correct_file/total*100:.1f}%")
    print(f"文档+页码都正确: {correct_both}/{total} = {correct_both/total*100:.1f}%")
    print(f"只选对文档但页码错误: {correct_file - correct_both}个")
    
    if error_cases:
        print(f"\n❌ 错误案例 (共{len(error_cases)}个):")
        for i, case in enumerate(error_cases[:5], 1):  # 只显示前5个
            print(f"\n{i}. {case['question']}")
            print(f"   预测: {case['pred_file']} 第{case['pred_page']}页")
            print(f"   正确: {case['gt_file']} 第{case['gt_page']}页")
            print(f"   文档{'✅' if case['file_match'] else '❌'} 页码{'✅' if False else '❌'}")
        
        if len(error_cases) > 5:
            print(f"\n   ... 还有 {len(error_cases)-5} 个错误案例")

def analyze_top50_recall(results: List[Dict], ground_truth: Dict):
    """分析Top50召回率"""
    print("\n" + "="*80)
    print("🔍 3. Top-K 召回率分析")
    print("="*80)
    
    topk_values = [1, 3, 5, 10, 20, 30, 50]
    
    for k in topk_values:
        file_recall = 0
        both_recall = 0
        total = 0
        
        file_ranks = []
        both_ranks = []
        
        for item in results:
            q = item['question']
            if q not in ground_truth:
                continue
            
            total += 1
            gt = ground_truth[q]
            retrieval_results = item.get('retrieval_results', [])
            
            # 检查TopK中是否有正确答案
            file_found = False
            both_found = False
            
            for rank, r in enumerate(retrieval_results[:k], 1):
                r_file = r.get('filename', '')
                r_page = r.get('page', 0)
                
                if r_file == gt['filename'] and not file_found:
                    file_found = True
                    file_recall += 1
                    file_ranks.append(rank)
                    
                    if r_page == gt['page'] and not both_found:
                        both_found = True
                        both_recall += 1
                        both_ranks.append(rank)
        
        print(f"\nTop-{k:2d}:")
        print(f"  文档召回: {file_recall}/{total} = {file_recall/total*100:.1f}%", end="")
        if file_ranks:
            print(f"  (平均排名: {sum(file_ranks)/len(file_ranks):.1f})")
        else:
            print()
        print(f"  完全召回: {both_recall}/{total} = {both_recall/total*100:.1f}%", end="")
        if both_ranks:
            print(f"  (平均排名: {sum(both_ranks)/len(both_ranks):.1f})")
        else:
            print()

def analyze_llm_rerank_performance(results: List[Dict], ground_truth: Dict):
    """分析LLM Rerank性能"""
    print("\n" + "="*80)
    print("🤖 4. LLM Rerank 性能分析")
    print("="*80)
    
    total = 0
    llm_correct_file = 0
    llm_correct_both = 0
    
    for item in results:
        q = item['question']
        if q not in ground_truth:
            continue
        
        total += 1
        gt = ground_truth[q]
        
        pred_file = item.get('filename', '')
        pred_page = item.get('page', 0)
        
        if pred_file == gt['filename']:
            llm_correct_file += 1
            if pred_page == gt['page']:
                llm_correct_both += 1
    
    print(f"\nLLM最终选择:")
    print(f"  文档准确率: {llm_correct_file}/{total} = {llm_correct_file/total*100:.1f}%")
    print(f"  完全准确率: {llm_correct_both}/{total} = {llm_correct_both/total*100:.1f}%")
    
    # 分析Top50召回 vs LLM最终准确率的gap
    top50_file = 0
    for item in results:
        q = item['question']
        if q not in ground_truth:
            continue
        
        gt = ground_truth[q]
        retrieval_results = item.get('retrieval_results', [])
        
        for r in retrieval_results[:50]:
            if r.get('filename') == gt['filename']:
                top50_file += 1
                break
    
    print(f"\n召回 vs 选择:")
    print(f"  Top50召回: {top50_file}/{total} = {top50_file/total*100:.1f}%")
    print(f"  LLM选择: {llm_correct_file}/{total} = {llm_correct_file/total*100:.1f}%")
    if top50_file > 0:
        print(f"  选择效率: {llm_correct_file/top50_file*100:.1f}% (在召回的基础上)")

def analyze_retrieval_distribution(results: List[Dict]):
    """分析检索结果分布"""
    print("\n" + "="*80)
    print("📈 5. 检索结果分布")
    print("="*80)
    
    retrieval_counts = []
    for item in results:
        retrieval_results = item.get('retrieval_results', [])
        retrieval_counts.append(len(retrieval_results))
    
    if retrieval_counts:
        print(f"\n每个问题检索到的结果数:")
        print(f"  最小: {min(retrieval_counts)}")
        print(f"  最大: {max(retrieval_counts)}")
        print(f"  平均: {sum(retrieval_counts)/len(retrieval_counts):.1f}")
        print(f"  中位数: {sorted(retrieval_counts)[len(retrieval_counts)//2]}")

def main():
    print("🔬 RAG系统性能全面分析")
    print("="*80)
    
    results, ground_truth = load_data()
    if not results or not ground_truth:
        return
    
    print(f"\n数据加载完成:")
    print(f"  问题总数: {len(results)}")
    print(f"  Ground Truth: {len(ground_truth)}")
    
    # 各项分析
    analyze_document_accuracy(results, ground_truth)
    analyze_top50_recall(results, ground_truth)
    analyze_llm_rerank_performance(results, ground_truth)
    analyze_retrieval_distribution(results)
    
    print("\n" + "="*80)
    print("✅ 分析完成")
    print("="*80)

if __name__ == "__main__":
    main()
