#!/usr/bin/env python3
"""
检索质量分析工具
用于分析logs目录下保存的详细检索结果
"""

import json
import os
import sys
from typing import Dict, List
from collections import defaultdict


def analyze_retrieval_quality(log_dir: str):
    """分析检索质量"""
    
    detailed_file = os.path.join(log_dir, "detailed_results.json")
    
    if not os.path.exists(detailed_file):
        print(f"❌ 找不到文件: {detailed_file}")
        return
    
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    print(f"\n{'='*80}")
    print(f"检索质量分析报告")
    print(f"日志目录: {log_dir}")
    print(f"问题总数: {len(results)}")
    print(f"{'='*80}\n")
    
    # 统计指标
    file_stats = defaultdict(int)  # 统计每个文件被检索到的次数
    rank1_correct = 0  # Rank1结果正确的次数
    top5_correct = 0   # Top5包含正确答案的次数
    
    for idx, item in enumerate(results, 1):
        question = item.get("question", "")
        answer_file = item.get("filename", "")
        answer_page = item.get("page", 0)
        retrieval_results = item.get("retrieval_results", [])
        
        print(f"\n📌 问题 {idx}: {question}")
        print(f"   预测: {answer_file} - 第{answer_page}页")
        
        if retrieval_results:
            print(f"   检索到 {len(retrieval_results)} 个结果:\n")
            
            # 检查是否正确
            rank1 = retrieval_results[0] if retrieval_results else {}
            if rank1.get("filename") == answer_file and rank1.get("page") == answer_page:
                rank1_correct += 1
            
            # 检查Top5是否包含正确答案
            for r in retrieval_results[:5]:
                if r.get("filename") == answer_file and r.get("page") == answer_page:
                    top5_correct += 1
                    break
            
            # 显示Top5检索结果
            for r in retrieval_results[:5]:
                rank = r.get("rank", 0)
                filename = r.get("filename", "")
                page = r.get("page", 0)
                score = r.get("score", 0.0)
                preview = r.get("content_preview", "")
                
                # 标记是否匹配预测结果
                match_mark = "✅" if (filename == answer_file and page == answer_page) else "  "
                
                print(f"   {match_mark} Rank {rank}: {filename} - 第{page}页")
                print(f"      Score: {score:.4f}")
                print(f"      预览: {preview[:100]}...")
                print()
                
                file_stats[filename] += 1
        else:
            print("   ⚠️  无检索结果")
    
    # 总体统计
    print(f"\n{'='*80}")
    print("总体统计")
    print(f"{'='*80}")
    print(f"Rank1准确率: {rank1_correct}/{len(results)} = {rank1_correct/len(results)*100:.2f}%")
    print(f"Top5召回率: {top5_correct}/{len(results)} = {top5_correct/len(results)*100:.2f}%")
    
    print(f"\n最常被检索到的文件 (Top 10):")
    sorted_files = sorted(file_stats.items(), key=lambda x: x[1], reverse=True)
    for filename, count in sorted_files[:10]:
        print(f"  - {filename}: {count}次")


def find_latest_log():
    """查找最新的日志目录"""
    logs_dir = "logs"
    if not os.path.exists(logs_dir):
        return None
    
    subdirs = [d for d in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, d))]
    if not subdirs:
        return None
    
    # 按时间戳排序，取最新的
    subdirs.sort(reverse=True)
    return os.path.join(logs_dir, subdirs[0])


if __name__ == "__main__":
    if len(sys.argv) > 1:
        log_dir = sys.argv[1]
    else:
        # 自动查找最新的日志
        log_dir = find_latest_log()
        if log_dir is None:
            print("❌ 找不到日志目录，请指定日志路径：")
            print("   python analyze_retrieval.py logs/20251121_230000")
            sys.exit(1)
        print(f"🔍 使用最新日志: {log_dir}")
    
    analyze_retrieval_quality(log_dir)
