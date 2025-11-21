#!/usr/bin/env python3
"""
深度分析召回质量：
检查召回的正确答案chunk的排名和内容质量
"""

import json
import os
from typing import Dict, List

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

def load_data():
    """加载数据"""
    with open('./data/answer.json', 'r', encoding='utf-8') as f:
        train_data = json.load(f)
    ground_truth = {item['question']: item for item in train_data}
    
    log_dir = find_latest_log()
    if not log_dir:
        return None, None
    
    detailed_file = os.path.join(log_dir, 'detailed_results.json')
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    return results, ground_truth

def analyze_recalled_quality():
    """分析召回的正确答案的质量"""
    results, ground_truth = load_data()
    if not results or not ground_truth:
        return
    
    print("=" * 100)
    print("🔍 召回质量深度分析：检查召回的正确答案chunk的相关性")
    print("=" * 100)
    
    recalled_but_wrong_selection = []  # 召回了但LLM没选对
    not_recalled = []  # 没召回
    recalled_and_selected = []  # 召回且LLM选对
    
    for item in results:
        q = item['question']
        if q not in ground_truth:
            continue
        
        gt = ground_truth[q]
        gt_file = gt['filename']
        gt_page = gt['page']
        
        pred_file = item.get('filename', '')
        pred_page = item.get('page', 0)
        
        retrieval_results = item.get('retrieval_results', [])
        
        # 查找正确答案在召回结果中的位置
        correct_chunks = []
        for rank, r in enumerate(retrieval_results, 1):
            if r.get('filename') == gt_file and r.get('page') == gt_page:
                correct_chunks.append({
                    'rank': rank,
                    'score': r.get('score', 0),
                    'content': r.get('content_preview', '')
                })
        
        llm_selected_correct = (pred_file == gt_file and pred_page == gt_page)
        
        if correct_chunks:
            if llm_selected_correct:
                recalled_and_selected.append({
                    'question': q,
                    'gt_file': gt_file.split('/')[-1],
                    'gt_page': gt_page,
                    'chunks': correct_chunks
                })
            else:
                recalled_but_wrong_selection.append({
                    'question': q,
                    'gt_file': gt_file.split('/')[-1],
                    'gt_page': gt_page,
                    'pred_file': pred_file.split('/')[-1] if pred_file else 'N/A',
                    'pred_page': pred_page,
                    'chunks': correct_chunks,
                    'top1_file': retrieval_results[0].get('filename', '').split('/')[-1] if retrieval_results else 'N/A',
                    'top1_page': retrieval_results[0].get('page', 0) if retrieval_results else 0,
                })
        else:
            not_recalled.append({
                'question': q,
                'gt_file': gt_file.split('/')[-1],
                'gt_page': gt_page,
                'pred_file': pred_file.split('/')[-1] if pred_file else 'N/A',
                'pred_page': pred_page,
            })
    
    total = len(results)
    
    # 统计
    print(f"\n📊 总体统计:")
    print(f"  总问题数: {total}")
    print(f"  召回且选对: {len(recalled_and_selected)} ({len(recalled_and_selected)/total*100:.1f}%)")
    print(f"  召回但选错: {len(recalled_but_wrong_selection)} ({len(recalled_but_wrong_selection)/total*100:.1f}%) ⚠️")
    print(f"  未召回: {len(not_recalled)} ({len(not_recalled)/total*100:.1f}%)")
    
    # 分析召回但选错的case - 这是关键瓶颈！
    if recalled_but_wrong_selection:
        print(f"\n" + "=" * 100)
        print(f"❌ 关键瓶颈：召回了但LLM选错的 {len(recalled_but_wrong_selection)} 个case")
        print("=" * 100)
        
        ranks = []
        scores = []
        
        for i, case in enumerate(recalled_but_wrong_selection, 1):
            chunks = case['chunks']
            best_chunk = chunks[0]  # 排名最靠前的正确chunk
            ranks.append(best_chunk['rank'])
            scores.append(best_chunk['score'])
            
            print(f"\n{i}. {case['question'][:70]}...")
            print(f"   正确答案: {case['gt_file']} 第{case['gt_page']}页")
            print(f"   LLM选择: {case['pred_file']} 第{case['pred_page']}页")
            print(f"   Top1结果: {case['top1_file']} 第{case['top1_page']}页")
            print(f"   正确chunk在Top50中的情况:")
            
            for chunk in chunks[:3]:  # 只显示前3个正确chunk
                print(f"      Rank {chunk['rank']:2d} | Score: {chunk['score']:.4f}")
                print(f"      内容: {chunk['content'][:150]}...")
                print()
        
        print(f"\n📈 正确答案排名统计:")
        print(f"   最高排名(最好): {min(ranks)}")
        print(f"   最低排名(最差): {max(ranks)}")
        print(f"   平均排名: {sum(ranks)/len(ranks):.1f}")
        print(f"   中位数排名: {sorted(ranks)[len(ranks)//2]}")
        
        print(f"\n📊 正确答案得分统计:")
        print(f"   最高分: {max(scores):.4f}")
        print(f"   最低分: {min(scores):.4f}")
        print(f"   平均分: {sum(scores)/len(scores):.4f}")
        
        # 分析排名分布
        rank_ranges = {
            'Top1-3': sum(1 for r in ranks if r <= 3),
            'Top4-10': sum(1 for r in ranks if 4 <= r <= 10),
            'Top11-20': sum(1 for r in ranks if 11 <= r <= 20),
            'Top21-50': sum(1 for r in ranks if r > 20),
        }
        
        print(f"\n📍 正确答案排名分布:")
        for range_name, count in rank_ranges.items():
            print(f"   {range_name}: {count}/{len(ranks)} = {count/len(ranks)*100:.1f}%")
    
    # 未召回的case
    if not_recalled:
        print(f"\n" + "=" * 100)
        print(f"⚠️ 未召回的 {len(not_recalled)} 个case")
        print("=" * 100)
        
        for i, case in enumerate(not_recalled, 1):
            print(f"\n{i}. {case['question'][:70]}...")
            print(f"   正确答案: {case['gt_file']} 第{case['gt_page']}页")
            print(f"   LLM选择: {case['pred_file']} 第{case['pred_page']}页")
    
    # 总结建议
    print(f"\n" + "=" * 100)
    print("💡 优化建议")
    print("=" * 100)
    
    if recalled_but_wrong_selection:
        avg_rank = sum(ranks) / len(ranks)
        if avg_rank > 10:
            print(f"\n❌ 问题1: 正确答案平均排名太低 ({avg_rank:.1f}位)")
            print("   建议: 优化检索算法，提升正确答案的排名")
            print("   - 调整向量检索和全文检索的权重")
            print("   - 优化RRF参数")
            print("   - 改进chunk分割策略")
        else:
            print(f"\n⚠️ 问题1: 正确答案排名尚可 ({avg_rank:.1f}位)，但LLM仍选错")
            print("   建议: 优化LLM Rerank策略")
            print("   - 减少候选数量(Top50→Top20)")
            print("   - 改进提示词")
            print("   - 提升content_preview质量")
        
        top10_count = sum(1 for r in ranks if r <= 10)
        if top10_count / len(ranks) < 0.7:
            print(f"\n❌ 问题2: 只有{top10_count}/{len(ranks)}个正确答案在Top10")
            print("   建议: 提升检索精准度，让更多正确答案排进Top10")
    
    if not_recalled:
        print(f"\n❌ 问题3: {len(not_recalled)}个问题完全未召回正确答案")
        print("   建议: 扩大召回范围或改进检索策略")

if __name__ == "__main__":
    analyze_recalled_quality()
