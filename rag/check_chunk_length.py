#!/usr/bin/env python3
"""
检查数据库中chunk的长度分布
"""

import os
from sqlalchemy import create_engine, text
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

TABLE_NAME = "rag_documents"

def get_db_engine():
    """Create SQLAlchemy engine for OceanBase."""
    uri = os.getenv("OCEANBASE_URI")
    user = os.getenv("OCEANBASE_USER")
    password = os.getenv("OCEANBASE_PASSWORD")
    db_name = os.getenv("OCEANBASE_DBNAME")
    
    # Parse URI (host:port)
    if ":" in uri:
        host, port = uri.split(":")
    else:
        host = uri
        port = "2881"
        
    connection_string = f"mysql+pymysql://{user}:{password}@{host}:{port}/{db_name}?charset=utf8mb4"
    return create_engine(connection_string, pool_pre_ping=True)

def check_chunk_lengths():
    """检查chunk长度统计"""
    engine = get_db_engine()
    
    print("=" * 100)
    print("📊 数据库Chunk长度分析")
    print("=" * 100)
    
    with engine.connect() as conn:
        # 总chunk数
        result = conn.execute(text(f"SELECT COUNT(*) FROM {TABLE_NAME}"))
        total_chunks = result.fetchone()[0]
        print(f"\n总Chunk数: {total_chunks}")
        
        # 长度统计
        result = conn.execute(text(f"""
            SELECT 
                MIN(CHAR_LENGTH(content)) as min_len,
                MAX(CHAR_LENGTH(content)) as max_len,
                AVG(CHAR_LENGTH(content)) as avg_len,
                STDDEV(CHAR_LENGTH(content)) as stddev_len
            FROM {TABLE_NAME}
        """))
        stats = result.fetchone()
        print(f"\n长度统计:")
        print(f"  最小长度: {stats[0]} 字符")
        print(f"  最大长度: {stats[1]} 字符")
        print(f"  平均长度: {stats[2]:.2f} 字符")
        print(f"  标准差: {stats[3]:.2f}")
        
        # 长度分布
        print(f"\n长度分布:")
        ranges = [
            (0, 500, "0-500"),
            (500, 1000, "500-1000"),
            (1000, 2000, "1000-2000"),
            (2000, 3000, "2000-3000"),
            (3000, 4000, "3000-4000"),
            (4000, 5000, "4000-5000"),
            (5000, 8192, "5000-8192"),
        ]
        
        for min_len, max_len, label in ranges:
            result = conn.execute(text(f"""
                SELECT COUNT(*) 
                FROM {TABLE_NAME} 
                WHERE CHAR_LENGTH(content) >= {min_len} AND CHAR_LENGTH(content) < {max_len}
            """))
            count = result.fetchone()[0]
            pct = count / total_chunks * 100 if total_chunks > 0 else 0
            bar = "█" * int(pct / 2)
            print(f"  {label:15s}: {count:6d} ({pct:5.1f}%) {bar}")
        
        # 最长的10个chunks
        print(f"\n" + "=" * 100)
        print("📄 最长的10个Chunks:")
        print("=" * 100)
        
        result = conn.execute(text(f"""
            SELECT 
                id,
                filename,
                page,
                CHAR_LENGTH(content) as len,
                LEFT(content, 200) as preview
            FROM {TABLE_NAME}
            ORDER BY CHAR_LENGTH(content) DESC
            LIMIT 10
        """))
        
        for idx, row in enumerate(result, 1):
            print(f"\n{idx}. ID: {row[0]}")
            print(f"   文件: {row[1].split('/')[-1] if row[1] else 'N/A'}")
            print(f"   页码: {row[2]}")
            print(f"   长度: {row[3]} 字符")
            print(f"   预览: {row[4]}...")
        
        # 最短的10个chunks
        print(f"\n" + "=" * 100)
        print("📄 最短的10个Chunks:")
        print("=" * 100)
        
        result = conn.execute(text(f"""
            SELECT 
                id,
                filename,
                page,
                CHAR_LENGTH(content) as len,
                content
            FROM {TABLE_NAME}
            ORDER BY CHAR_LENGTH(content) ASC
            LIMIT 10
        """))
        
        for idx, row in enumerate(result, 1):
            print(f"\n{idx}. ID: {row[0]}")
            print(f"   文件: {row[1].split('/')[-1] if row[1] else 'N/A'}")
            print(f"   页码: {row[2]}")
            print(f"   长度: {row[3]} 字符")
            print(f"   内容: {row[4]}")
        
        # 接近8192上限的chunks
        print(f"\n" + "=" * 100)
        print("⚠️  接近8192字符上限的Chunks (>7000字符):")
        print("=" * 100)
        
        result = conn.execute(text(f"""
            SELECT 
                id,
                filename,
                page,
                CHAR_LENGTH(content) as len
            FROM {TABLE_NAME}
            WHERE CHAR_LENGTH(content) > 7000
            ORDER BY CHAR_LENGTH(content) DESC
        """))
        
        rows = result.fetchall()
        if rows:
            for idx, row in enumerate(rows, 1):
                print(f"{idx}. ID: {row[0]}, 文件: {row[1].split('/')[-1] if row[1] else 'N/A'}, "
                      f"页码: {row[2]}, 长度: {row[3]}")
        else:
            print("✅ 没有接近上限的chunks")
    
    print(f"\n" + "=" * 100)
    print("✅ 分析完成")
    print("=" * 100)

if __name__ == "__main__":
    check_chunk_lengths()
