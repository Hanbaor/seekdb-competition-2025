import json

with open('./data/output.json', 'r') as f:
    output = json.load(f)
with open('./data/answer.json', 'r') as f:
    answer = json.load(f)

print('=' * 80)
print('逐题对比分析')
print('=' * 80)

for i, (o, a) in enumerate(zip(output, answer), 1):
    q = a["question"][:40]
    print(f'\n【问题 {i}】{q}...')
    print(f'  标准答案: {a["answer"][:60]}...')
    print(f'  生成答案: {o["answer"][:60]}...')
    
    file_match = 'Y' if o['filename'] == a['filename'] else 'N'
    page_match = 'Y' if o['page'] == a['page'] else 'N'
    
    print(f'  文件匹配: {file_match} | 预测={o["filename"][:40]}')
    print(f'                    | 标准={a["filename"][:40]}')
    print(f'  页码匹配: {page_match} | 预测={o["page"]} | 标准={a["page"]}')

print('\n' + '=' * 80)
print('统计汇总')
print('=' * 80)
file_correct = sum(1 for o, a in zip(output, answer) if o['filename'] == a['filename'])
page_correct = sum(1 for o, a in zip(output, answer) if o['filename'] == a['filename'] and o['page'] == a['page'])
print(f'文件匹配正确: {file_correct}/{len(answer)}')
print(f'页码匹配正确: {page_correct}/{len(answer)}')
