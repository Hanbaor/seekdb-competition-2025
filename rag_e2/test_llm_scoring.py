#!/usr/bin/env python3
"""测试LLM评分功能"""
import json
import sys
import yaml
from dotenv import load_dotenv
from api_client import APIClient

# 加载环境变量
load_dotenv()

# 加载配置
with open('competition_config.yaml', 'r', encoding='utf-8') as f:
    config = yaml.safe_load(f)

# 初始化API客户端
api_client = APIClient(config)

def test_llm_scoring():
    """测试LLM对文档相关性的评分"""
    
    # 测试案例：Q2的正确文档 vs 高分文档
    question = "千味央厨的财务报表预测中，预计2024年的营业收入是多少？"
    
    # 加载debug数据
    d = json.load(open('debug_output/question_02.json'))
    
    # 找到正确文档和一个高分文档
    correct_doc = '千味央厨-首次覆盖报告受益餐饮工业化趋势大B端壁垒赋能小B端降维打击-22052929页'
    
    correct_retrieval = None
    high_score_retrieval = None
    
    for r in d['retrievals']:
        if correct_doc in r['doc_name']:
            correct_retrieval = r
        elif r.get('retrieval_score', 0) == 6 and not high_score_retrieval:
            high_score_retrieval = r
    
    print("=" * 80)
    print("LLM相关性评分测试")
    print("=" * 80)
    print(f"\n问题: {question}")
    print()
    
    # 测试正确文档
    print("-" * 80)
    print(f"✅ 正确文档: {correct_retrieval['doc_name'][:60]}...")
    print(f"原得分: {correct_retrieval['retrieval_score']} (基于chunk数量)")
    
    eval_text = correct_retrieval['chunks_text'][:3000]
    if len(correct_retrieval['chunks_text']) > 3000:
        eval_text += "...(文本已截断)"
    
    relevance_prompt = f"""你是一个文档相关性评估专家。请评估以下检索到的文档片段与用户问题的相关性。

问题：{question}

文档片段：
{eval_text}

请给出一个0到1之间的相关性分数：
- 0.9-1.0: 高度相关，文档内容直接回答了问题
- 0.7-0.89: 较相关，文档包含相关信息但可能不够完整
- 0.5-0.69: 中等相关，文档有部分相关内容
- 0.3-0.49: 弱相关，文档只有少量相关信息
- 0.0-0.29: 不相关，文档与问题无关

只返回一个浮点数分数，不要任何解释。"""
    
    try:
        llm_response = api_client.call_llm(
            messages=[{"role": "user", "content": relevance_prompt}],
            temperature=0.1,
            max_tokens=10
        )
        score = float(llm_response.strip())
        print(f"LLM评分: {score:.3f}")
        print(f"文档预览: {correct_retrieval['chunks_text'][:200]}...")
    except Exception as e:
        print(f"评分失败: {e}")
    
    print()
    
    # 测试高分文档
    print("-" * 80)
    print(f"🔴 高分文档: {high_score_retrieval['doc_name'][:60]}...")
    print(f"原得分: {high_score_retrieval['retrieval_score']} (基于chunk数量)")
    
    eval_text = high_score_retrieval['chunks_text'][:3000]
    if len(high_score_retrieval['chunks_text']) > 3000:
        eval_text += "...(文本已截断)"
    
    relevance_prompt = f"""你是一个文档相关性评估专家。请评估以下检索到的文档片段与用户问题的相关性。

问题：{question}

文档片段：
{eval_text}

请给出一个0到1之间的相关性分数：
- 0.9-1.0: 高度相关，文档内容直接回答了问题
- 0.7-0.89: 较相关，文档包含相关信息但可能不够完整
- 0.5-0.69: 中等相关，文档有部分相关内容
- 0.3-0.49: 弱相关，文档只有少量相关信息
- 0.0-0.29: 不相关，文档与问题无关

只返回一个浮点数分数，不要任何解释。"""
    
    try:
        llm_response = api_client.call_llm(
            messages=[{"role": "user", "content": relevance_prompt}],
            temperature=0.1,
            max_tokens=10
        )
        score = float(llm_response.strip())
        print(f"LLM评分: {score:.3f}")
        print(f"文档预览: {high_score_retrieval['chunks_text'][:200]}...")
    except Exception as e:
        print(f"评分失败: {e}")
    
    print()
    print("=" * 80)
    print("结论:")
    print("  如果LLM给正确文档的分数 > 高分文档，则LLM评分有效！")
    print("=" * 80)

if __name__ == "__main__":
    test_llm_scoring()
