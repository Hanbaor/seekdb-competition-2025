#!/usr/bin/env python3
"""分析调试输出文件"""
import json
from pathlib import Path
import sys

def analyze_question(debug_file):
    """分析单个问题的调试信息"""
    with open(debug_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    print("\n" + "="*80)
    print(f"问题 {data['question_id']}: {data['question']}")
    print("="*80)
    
    # 检索统计
    print(f"\n📊 检索统计:")
    print(f"  检索文档数: {len(data['retrievals'])}")
    success_count = sum(1 for r in data['retrievals'] if r.get('has_result', False))
    print(f"  成功返回结果: {success_count}/{len(data['retrievals'])}")
    print(f"  最终使用文档数: {data['num_docs_retrieved']}")
    
    # 检索详情
    print(f"\n🔍 各文档检索详情:")
    for i, ret in enumerate(data['retrievals'], 1):
        if 'error' in ret:
            print(f"\n  [{i}] ❌ {ret['doc_name']}")
            print(f"      错误: {ret['error']}")
        else:
            status = "✅" if ret['has_result'] else "❌"
            print(f"\n  [{i}] {status} {ret['doc_name']}")
            print(f"      检索类型: {ret.get('retrieval_type', 'unknown')}")
            print(f"      提取实体: {ret.get('entities', [])}") 
            print(f"      返回文本长度: {ret.get('chunks_length', 0)} 字符")
            if ret.get('chunks_text'):
                preview = ret['chunks_text'][:200].replace('\n', ' ')
                print(f"      内容预览: {preview}...")
    
    # 上下文统计
    print(f"\n📝 上下文信息:")
    print(f"  总长度: {data['context_length']} 字符")
    print(f"  Prompt总长度: {data['prompt_length']} 字符")
    
    # 来源归因
    print(f"\n📍 来源归因:")
    print(f"  文件: {data['source']['filename']}")
    print(f"  页码: {data['source']['page']}")
    
    # 答案
    print(f"\n💡 生成答案:")
    answer_preview = data['answer'][:300]
    print(f"  {answer_preview}...")
    print(f"  (完整答案长度: {len(data['answer'])} 字符)")
    
    return data

def analyze_all():
    """分析所有调试文件"""
    debug_dir = Path("./debug_output")
    if not debug_dir.exists():
        print("❌ debug_output 目录不存在")
        return
    
    files = sorted(debug_dir.glob("question_*.json"))
    if not files:
        print("❌ 没有找到调试文件")
        return
    
    print(f"找到 {len(files)} 个调试文件\n")
    
    # 统计信息
    total_questions = len(files)
    total_retrievals = 0
    successful_retrievals = 0
    retrieval_types = {}
    doc_usage = {}
    
    for f in files:
        data = analyze_question(f)
        
        # 统计
        for ret in data['retrievals']:
            total_retrievals += 1
            if ret.get('has_result'):
                successful_retrievals += 1
                doc_name = ret['doc_name']
                doc_usage[doc_name] = doc_usage.get(doc_name, 0) + 1
            
            ret_type = ret.get('retrieval_type', 'unknown')
            retrieval_types[ret_type] = retrieval_types.get(ret_type, 0) + 1
    
    # 总结
    print("\n" + "="*80)
    print("📊 总体统计")
    print("="*80)
    print(f"总问题数: {total_questions}")
    print(f"总检索次数: {total_retrievals}")
    print(f"成功检索: {successful_retrievals}/{total_retrievals} ({successful_retrievals/total_retrievals*100:.1f}%)")
    
    print(f"\n检索类型分布:")
    for rtype, count in sorted(retrieval_types.items(), key=lambda x: -x[1]):
        print(f"  {rtype}: {count}")
    
    print(f"\n最常被检索到的文档 (Top 10):")
    for doc, count in sorted(doc_usage.items(), key=lambda x: -x[1])[:10]:
        print(f"  {doc}: {count} 次")

def show_context(question_id):
    """显示指定问题的完整上下文"""
    debug_file = Path(f"./debug_output/question_{question_id:02d}.json")
    if not debug_file.exists():
        print(f"❌ 文件不存在: {debug_file}")
        return
    
    with open(debug_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    print(f"\n问题 {question_id}: {data['question']}")
    print("\n" + "="*80)
    print("完整上下文:")
    print("="*80)
    print(data['context'])
    print("\n" + "="*80)
    print("完整 Prompt:")
    print("="*80)
    print(data['prompt'])

if __name__ == "__main__":
    if len(sys.argv) > 1:
        if sys.argv[1] == "context":
            if len(sys.argv) > 2:
                show_context(int(sys.argv[2]))
            else:
                print("用法: python analyze_debug.py context <question_id>")
        else:
            print("用法:")
            print("  python analyze_debug.py           # 分析所有问题")
            print("  python analyze_debug.py context N # 显示问题N的完整上下文")
    else:
        analyze_all()
