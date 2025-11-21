#!/usr/bin/env python3
"""测试模糊匹配功能"""
import json
import sys

# 手动添加path
sys.path.insert(0, 'E2GraphRAG-main')
import jieba.analyse

def test_fuzzy_matching():
    # 加载一个graph的index
    graph_path = 'cache/seekdb_competition/千味央厨-公司研究报告-深耕餐饮供应链为人间千味-23011258页'
    try:
        with open(f'{graph_path}/index.json', 'r', encoding='utf-8') as f:
            index = json.load(f)
    except Exception as e:
        print(f"无法加载graph: {e}")
        return
    
    # 测试问题
    test_cases = [
        "千味央厨的成立时间是什么时候？",
        "千味央厨的财务报表预测中，预计2024年的营业收入是多少？",
        "千味央厨的主要产品有哪些类别？",
    ]
    
    stopwords = {'什么', '如何', '哪些', '多少', '怎样', '为什么', '是', '的', '了', '在', '与', '和'}
    
    for query in test_cases:
        print("=" * 80)
        print(f"问题: {query}")
        print("=" * 80)
        
        # jieba提取
        raw_entities = jieba.analyse.extract_tags(query, topK=5, withWeight=False)
        print(f"jieba提取: {raw_entities}")
        
        # 模糊匹配
        entities = []
        for entity in raw_entities:
            if entity in stopwords or len(entity) < 2:
                print(f"  ❌ '{entity}': 停用词，跳过")
                continue
                
            if entity in index:
                print(f"  ✅ '{entity}': 精确匹配")
                entities.append(entity)
            else:
                # 模糊匹配
                matches = []
                for graph_entity in index.keys():
                    # 过滤空字符串和过短的实体
                    if not graph_entity or len(graph_entity) < 2:
                        continue
                        
                    if entity in graph_entity or graph_entity in entity:
                        if entity in graph_entity:
                            score = len(entity) / len(graph_entity)
                        else:
                            score = len(graph_entity) / len(entity)
                        matches.append((graph_entity, score))
                
                if matches:
                    matches.sort(key=lambda x: x[1], reverse=True)
                    best_match, best_score = matches[0]
                    # 只接受得分 > 0.2 的匹配
                    if best_score > 0.2:
                        print(f"  🔍 '{entity}': 模糊匹配 → '{best_match}' (score={best_score:.3f})")
                        entities.append(best_match)
                        if len(matches) > 1:
                            others = ', '.join([f"{m[0][:20]}({m[1]:.2f})" for m in matches[1:min(4, len(matches))]])
                            print(f"     其他候选: {others}")
                    else:
                        print(f"  ⚠️  '{entity}': 得分过低 ({best_score:.3f}), 跳过")
                else:
                    print(f"  ❌ '{entity}': 无匹配")
        
        valid_raw = [e for e in raw_entities if e not in stopwords and len(e) >= 2]
        print(f"\n  最终匹配实体: {entities}")
        print(f"  成功率: {len(entities)}/{len(valid_raw)} = {len(entities)/len(valid_raw)*100 if valid_raw else 0:.1f}%")
        print()

if __name__ == "__main__":
    test_fuzzy_matching()
