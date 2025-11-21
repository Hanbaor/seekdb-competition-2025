#!/usr/bin/env python3
"""检查数据库中是否存在正确答案所在的chunk"""

import json
import os
from pathlib import Path
from sqlalchemy import create_engine, text
import dotenv

dotenv.load_dotenv()

# Load ground truth
gt_file = Path(__file__).parent / 'data' / 'answer.json'
with open(gt_file, 'r', encoding='utf-8') as f:
    gt_data = json.load(f)

ground_truth = {item['question']: item for item in gt_data}

# Connect to database
def get_db_engine():
    uri = os.getenv("OCEANBASE_URI")
    user = os.getenv("OCEANBASE_USER")
    password = os.getenv("OCEANBASE_PASSWORD")
    db_name = os.getenv("OCEANBASE_DBNAME")
    
    if ":" in uri:
        host, port = uri.split(":")
    else:
        host = uri
        port = "2881"
    
    connection_string = f"mysql+pymysql://{user}:{password}@{host}:{port}/{db_name}?charset=utf8mb4"
    return create_engine(connection_string)

# 一直错误的题目
always_wrong = [
    "千味央厨的成立时间是什么时候？",
    "千味央厨的财务报表预测中，预计2024年的营业收入是多少？",
    "千味央厨的主要产品有哪些类别？",
    "千味央厨在2021年的营业收入是多少？",
    "千味央厨的销售费用率和管理费用率如何，主要原因是什么？",
]

engine = get_db_engine()

print("=" * 100)
print("检查一直错误题目的chunk是否在数据库中")
print("=" * 100)

for question in always_wrong:
    gt = ground_truth[question]
    gt_filename = gt['filename']
    gt_page = gt['page']
    gt_answer = gt['answer']
    
    print(f"\n{'='*100}")
    print(f"❓ 问题: {question}")
    print(f"📄 正确答案应在: {gt_filename} 第{gt_page}页")
    print(f"✅ 正确答案: {gt_answer[:100]}...")
    
    # Query database for chunks from this file and page
    query = text("""
        SELECT id, filename, page, LENGTH(content) as content_len, 
               SUBSTRING(content, 1, 200) as content_preview
        FROM rag_documents
        WHERE filename = :filename AND page = :page
        ORDER BY id
    """)
    
    with engine.connect() as conn:
        results = conn.execute(query, {"filename": gt_filename, "page": gt_page}).fetchall()
    
    if not results:
        print(f"❌ 数据库中找不到这个文件和页码的chunk!")
        print(f"   可能原因: VLM解析时跳过了这一页，或者chunk分割时丢失")
    else:
        print(f"✅ 找到 {len(results)} 个chunk:")
        for idx, row in enumerate(results, 1):
            print(f"   Chunk {idx}: ID={row[0]}, 长度={row[3]}, 预览: {row[4][:100]}...")
    
    # Also check if there are any chunks from this file
    query_file = text("""
        SELECT COUNT(*) as cnt, MIN(page) as min_page, MAX(page) as max_page
        FROM rag_documents
        WHERE filename = :filename
    """)
    
    with engine.connect() as conn:
        file_stats = conn.execute(query_file, {"filename": gt_filename}).fetchone()
    
    if file_stats[0] == 0:
        print(f"❌ 整个文件 {gt_filename} 都不在数据库中!")
    else:
        print(f"📊 该文件在数据库中有 {file_stats[0]} 个chunk, 页码范围: {file_stats[1]} - {file_stats[2]}")
        if gt_page < file_stats[1] or gt_page > file_stats[2]:
            print(f"⚠️  正确答案的页码({gt_page})不在数据库的页码范围内!")

print(f"\n{'='*100}")
