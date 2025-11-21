#!/usr/bin/env python3
"""深度分析检索问题"""
import json
from pathlib import Path
from collections import Counter

def main():
    # 加载标准答案
    with open('./data/answer.json', 'r', encoding='utf-8') as f:
        answers = json.load(f)
    
    # 加载debug输出
    debug_files = sorted(Path('./debug_output').glob('question_*.json'))
    
    print("="*80)
    print("检索问题深度分析")
    print("="*80)
    
    # 统计1: 实体提取情况
    print("\n📊 1. 实体提取统计")
    print("-"*80)
    total_retrievals = 0
    empty_entities = 0
    retrieval_types = Counter()
    
    for debug_file in debug_files:
        with open(debug_file, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        for ret in data['retrievals']:
            total_retrievals += 1
            retrieval_types[ret.get('retrieval_type', 'unknown')] += 1
            if not ret.get('entities'):
                empty_entities += 1
    
    print(f"总检索次数: {total_retrievals}")
    print(f"实体为空: {empty_entities} ({empty_entities/total_retrievals*100:.1f}%)")
    print(f"\n检索类型分布:")
    for rtype, count in retrieval_types.most_common():
        print(f"  {rtype}: {count} ({count/total_retrievals*100:.1f}%)")
    
    # 统计2: 文件名匹配分析
    print("\n📊 2. 文件名匹配分析")
    print("-"*80)
    correct_filename = 0
    correct_page = 0
    
    for i, (answer, debug_file) in enumerate(zip(answers, debug_files), 1):
        with open(debug_file, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        expected_file = answer['filename'].replace('.pdf', '')
        expected_page = answer['page']
        actual_file = data['source']['filename'].replace('.pdf', '')
        actual_page = data['source']['page']
        
        file_match = expected_file == actual_file
        page_match = expected_page == actual_page
        
        if file_match:
            correct_filename += 1
        if file_match and page_match:
            correct_page += 1
        
        status = "✅" if file_match else "❌"
        print(f"\nQ{i}: {status} {data['question'][:40]}...")
        print(f"  预期: {expected_file[:60]}... (P{expected_page})")
        print(f"  实际: {actual_file[:60]}... (P{actual_page})")
        
        # 检查是否检索到正确文档
        retrieved_docs = [r['doc_name'] for r in data['retrievals'] if r.get('has_result')]
        if expected_file in retrieved_docs:
            rank = retrieved_docs.index(expected_file) + 1
            print(f"  ⚠️  正确文档已检索到，但排名第{rank}（共{len(retrieved_docs)}个）")
        else:
            print(f"  ❌ 正确文档未被检索到")
    
    print(f"\n文件名匹配率: {correct_filename}/{len(answers)} = {correct_filename/len(answers)*100:.1f}%")
    print(f"页码匹配率: {correct_page}/{len(answers)} = {correct_page/len(answers)*100:.1f}%")
    
    # 统计3: 检索到的文档分布
    print("\n📊 3. 被选为来源的文档分布")
    print("-"*80)
    source_docs = Counter()
    for debug_file in debug_files:
        with open(debug_file, 'r', encoding='utf-8') as f:
            data = json.load(f)
        doc = data['source']['filename'].replace('.pdf', '')
        if doc:
            source_docs[doc] += 1
    
    for doc, count in source_docs.most_common():
        print(f"  {doc[:60]}: {count}次")
    
    # 统计4: 具体案例分析
    print("\n📊 4. 典型失败案例分析")
    print("-"*80)
    
    # 看第一个问题
    with open(debug_files[0], 'r', encoding='utf-8') as f:
        q1 = json.load(f)
    
    print(f"\n案例: {q1['question']}")
    print(f"预期文档: 千味央厨-公司研究报告-深耕餐饮供应链为人间千味-23011258页")
    print(f"实际使用: {q1['source']['filename'].replace('.pdf', '')}")
    
    # 找到这个文档的检索情况
    for ret in q1['retrievals']:
        if '深耕餐饮供应链为人间千味' in ret['doc_name']:
            print(f"\n该文档的检索详情:")
            print(f"  检索类型: {ret['retrieval_type']}")
            print(f"  提取实体: {ret['entities']}")
            print(f"  返回chunks: {ret['chunks_length']} 字符")
            print(f"  chunk_ids: {list(ret['chunk_ids'].get('', []))}")
            print(f"  内容预览: {ret['chunks_text'][:200]}...")
            break
    
    # 统计5: 上下文长度分析
    print("\n📊 5. 上下文长度统计")
    print("-"*80)
    context_lengths = []
    for debug_file in debug_files:
        with open(debug_file, 'r', encoding='utf-8') as f:
            data = json.load(f)
        context_lengths.append(data['context_length'])
    
    print(f"平均上下文长度: {sum(context_lengths)/len(context_lengths):.0f} 字符")
    print(f"最小: {min(context_lengths)} 字符")
    print(f"最大: {max(context_lengths)} 字符")
    print(f"每个问题检索文档数: {data['num_docs_retrieved']} (配置: max_chunk_setting=3)")

if __name__ == "__main__":
    main()
