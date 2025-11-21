#!/usr/bin/env python3
"""测试LLM reranking能否选择正确文档"""
import json
import yaml
import asyncio
from dotenv import load_dotenv
from api_client import APIClient

# 加载环境变量和配置
load_dotenv()
with open('competition_config.yaml', 'r', encoding='utf-8') as f:
    config = yaml.safe_load(f)

api_client = APIClient(config)

# 测试案例
test_cases = [
    {
        "question_id": 3,
        "question": '千味央厨面临的餐饮业"三高一低"困境是什么？',
        "correct_docs": ["千味央厨-投资价值分析报告滴水穿石觅百厨千味-22021137页"]
    },
    {
        "question_id": 6,
        "question": "千味央厨在2021年的营业收入是多少？",
        "correct_docs": ["千味央厨-投资价值分析报告滴水穿石觅百厨千味-22021137页"]
    },
    {
        "question_id": 14,
        "question": "千味央厨的财务报表预测中，预计2024年的营业收入是多少？",
        "correct_docs": ["千味央厨-首次覆盖报告受益餐饮工业化趋势大B端壁垒赋能小B端降维打击-22052929页"]
    }
]

async def test_reranking(test_case):
    """测试单个问题的reranking"""
    question_id = test_case["question_id"]
    question = test_case["question"]
    correct_docs = test_case["correct_docs"]
    
    # 加载debug数据
    debug_file = f'debug_output/question_{question_id:02d}.json'
    try:
        with open(debug_file, 'r', encoding='utf-8') as f:
            d = json.load(f)
    except FileNotFoundError:
        print(f"❌ 文件不存在: {debug_file}")
        return None
    
    print("=" * 80)
    print(f"测试 Q{question_id}: {question}")
    print("=" * 80)
    
    # 构建候选文档列表
    candidates_text = []
    valid_retrievals = [r for r in d['retrievals'] if r.get('chunks_text')]
    
    print(f"\n📚 候选文档数量: {len(valid_retrievals)}")
    
    for idx, r in enumerate(valid_retrievals):
        doc_name = r['doc_name']
        chunks = r['chunks_text']
        candidates_text.append(f"[文档{idx+1}] {doc_name}\n{chunks}")
        
        # 标记正确文档
        is_correct = any(correct in doc_name for correct in correct_docs)
        marker = "✅ 正确" if is_correct else ""
        print(f"  [{idx+1:2d}] {doc_name[:60]:60s} {len(chunks):6d}字符 {marker}")
    
    if not candidates_text:
        print("❌ 没有候选文档")
        return None
    
    # 构建LLM选择prompt
    selection_prompt = f"""你是一个文档相关性评估专家。用户提出了一个问题，以下是从多个文档中检索到的内容片段。

问题：{question}

候选文档：
{'=' * 60}
{chr(10).join(candidates_text)}
{'=' * 60}

请评估每个文档与问题的相关性，并返回最相关的那个文档编号。

要求：
1. 只返回一个数字（文档编号，从1开始），例如："3"
2. 如果没有相关文档，返回"0"
3. 不要返回任何其他解释或文字

最相关的文档编号是："""
    
    print(f"\n🤖 调用LLM进行reranking...")
    print(f"   Prompt长度: {len(selection_prompt):,} 字符")
    print(f"   预估tokens: {len(selection_prompt) // 2:,}")
    
    try:
        # 调用LLM
        llm_response = await api_client.call_vlm(
            messages=[{"role": "user", "content": selection_prompt}]
        )
        
        print(f"\n📝 LLM响应: '{llm_response.strip()}'")
        
        # 解析结果
        selected_idx = int(llm_response.strip()) - 1
        
        if 0 <= selected_idx < len(valid_retrievals):
            selected_doc = valid_retrievals[selected_idx]['doc_name']
            print(f"\n🎯 LLM选择: [文档{selected_idx+1}] {selected_doc}")
            
            # 检查是否正确
            is_correct = any(correct in selected_doc for correct in correct_docs)
            
            if is_correct:
                print(f"✅ 选择正确！")
                return True
            else:
                print(f"❌ 选择错误！")
                print(f"   正确文档应该包含: {correct_docs}")
                
                # 找到正确文档的位置
                for idx, r in enumerate(valid_retrievals):
                    if any(correct in r['doc_name'] for correct in correct_docs):
                        print(f"   正确文档在列表中的位置: [文档{idx+1}]")
                        break
                
                return False
        else:
            print(f"❌ LLM返回的索引无效: {selected_idx}")
            return None
            
    except Exception as e:
        print(f"❌ LLM调用失败: {e}")
        import traceback
        traceback.print_exc()
        return None

async def main():
    """测试所有案例"""
    print("\n" + "=" * 80)
    print("LLM Reranking 准确性测试")
    print("=" * 80)
    
    results = []
    for test_case in test_cases:
        result = await test_reranking(test_case)
        results.append(result)
        print()
    
    # 统计
    print("\n" + "=" * 80)
    print("测试结果汇总")
    print("=" * 80)
    
    valid_results = [r for r in results if r is not None]
    if valid_results:
        correct_count = sum(1 for r in valid_results if r)
        total_count = len(valid_results)
        accuracy = correct_count / total_count * 100
        
        print(f"\n准确率: {correct_count}/{total_count} = {accuracy:.1f}%")
        
        for i, (test_case, result) in enumerate(zip(test_cases, results)):
            if result is not None:
                status = "✅" if result else "❌"
                print(f"  {status} Q{test_case['question_id']}: {test_case['question'][:50]}...")
    else:
        print("❌ 没有有效的测试结果")

if __name__ == "__main__":
    asyncio.run(main())
