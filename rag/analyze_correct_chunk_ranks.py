#!/usr/bin/env python3
"""分析正确chunk在召回结果中的排名"""

import json
import sys
from pathlib import Path

def analyze_ranks(log_dir: str):
    log_path = Path(log_dir)
    if not log_path.exists():
        logs_dir = Path(__file__).parent / 'logs'
        all_logs = sorted([d for d in logs_dir.iterdir() if d.is_dir()], reverse=True)
        if all_logs:
            log_path = all_logs[0]
            print(f"使用最新运行: {log_path.name}\n")
    
    # Load ground truth
    gt_file = Path(__file__).parent / 'data' / 'answer.json'
    with open(gt_file, 'r', encoding='utf-8') as f:
        gt_data = json.load(f)
    ground_truth = {item['question']: item for item in gt_data}
    
    # Load detailed results
    detailed_file = log_path / 'detailed_results.json'
    with open(detailed_file, 'r', encoding='utf-8') as f:
        results = json.load(f)
    
    print("=" * 100)
    print("分析正确chunk在召回结果中的排名")
    print("=" * 100)
    
    # 一直错误的题目
    always_wrong = [
        "千味央厨的成立时间是什么时候？",
        "千味央厨的财务报表预测中，预计2024年的营业收入是多少？",
        "千味央厨的主要产品有哪些类别？",
        "千味央厨在2021年的营业收入是多少？",
        "千味央厨的销售费用率和管理费用率如何，主要原因是什么？",
    ]
    
    for question in always_wrong:
        gt = ground_truth[question]
        gt_filename = gt['filename']
        gt_page = gt['page']
        
        # Find this question in results
        result = None
        for r in results:
            if r['question'] == question:
                result = r
                break
        
        if not result:
            continue
        
        print(f"\n{'='*100}")
        print(f"❓ 问题: {question}")
        print(f"📄 正确答案在: {gt_filename} 第{gt_page}页")
        
        # Check all retrieval results
        retrieval_results = result.get('retrieval_results', [])
        found_ranks = []
        
        for ret in retrieval_results:
            if ret['filename'] == gt_filename and ret['page'] == gt_page:
                found_ranks.append({
                    'rank': ret['rank'],
                    'score': ret['score'],
                    'source': ret.get('source', 'unknown'),
                    'preview': ret.get('content_preview', '')[:100]
                })
        
        if not found_ranks:
            print(f"❌ 正确chunk未出现在召回结果中 (可能排名>{len(retrieval_results)})")
            print(f"   召回结果总数: {len(retrieval_results)}")
        else:
            print(f"✅ 找到 {len(found_ranks)} 个正确chunk:")
            for fr in found_ranks:
                print(f"   排名#{fr['rank']}: score={fr['score']:.2f}, source={fr['source']}")
                print(f"   内容预览: {fr['preview']}...")
        
        # Show top 10 retrieved chunks with FULL content for analysis
        print(f"\n📊 Top 10 召回chunk的完整内容分析:")
        print(f"正确答案: {gt['answer']}\n")
        
        for ret in retrieval_results[:10]:
            marker = "✅✅" if (ret['filename'] == gt_filename and ret['page'] == gt_page) else "  "
            print(f"\n{marker} 【排名#{ret['rank']}】{ret['filename'][:60]} 第{ret['page']}页")
            print(f"   Score: {ret['score']:.2f}, Source: {ret.get('source', 'unknown')}")
            
            # Show full content if available
            content = ret.get('content_preview', '')
            if content and len(content) > 0:
                print(f"   内容: {content}")
            else:
                print(f"   (无内容预览)")
            
            # Check if ground truth answer keywords appear in this chunk
            answer_text = gt['answer']
            # Extract key numbers/keywords from answer
            if any(keyword in content for keyword in ['2012', '2024', '2021', '油炸', '烘焙', '蒸煮', '菜肴', '销售费用', '管理费用']):
                print(f"   ⚠️  包含答案关键词！")

if __name__ == '__main__':
    log_dir = sys.argv[1] if len(sys.argv) > 1 else 'logs'
    analyze_ranks(log_dir)
