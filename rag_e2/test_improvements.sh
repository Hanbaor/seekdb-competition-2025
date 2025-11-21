#!/bin/bash
# 测试改进效果

echo "======================================"
echo "测试 jieba NER + 源归因修复"
echo "======================================"

# 1. 清除旧的debug输出
echo -e "\n[1/4] 清除旧的debug输出..."
rm -rf debug_output/*

# 2. 重新运行查询（跳过indexing）
echo -e "\n[2/4] 重新运行查询..."
python main.py --config competition_config.yaml --questions ./data/questions.json --skip-ingest

# 3. 分析结果
echo -e "\n[3/4] 分析检索质量..."
python analyze_retrieval_issues.py 2>&1 | head -80

# 4. 快速对比
echo -e "\n[4/4] 快速评估..."
python -c "
import json

output = json.load(open('data/output.json'))
answer = json.load(open('data/answer.json'))

correct_filename = 0
correct_page = 0

for o, a in zip(output, answer):
    o_file = o['filename'].replace('.pdf', '')
    a_file = a['filename'].replace('.pdf', '')
    if o_file == a_file:
        correct_filename += 1
        if o['page'] == a['page']:
            correct_page += 1

print(f'\n文件名匹配: {correct_filename}/{len(output)} = {correct_filename/len(output)*100:.1f}%')
print(f'页码匹配: {correct_page}/{len(output)} = {correct_page/len(output)*100:.1f}%')
print(f'\n改进前: 文件名7.1%, 页码0%')
print(f'改进后: 文件名{correct_filename/len(output)*100:.1f}%, 页码{correct_page/len(output)*100:.1f}%')
"

echo -e "\n======================================"
echo "测试完成！"
echo "======================================"
