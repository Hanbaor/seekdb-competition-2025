"""
测试 text-embedding-async-v2 异步批量 Embedding API
使用OpenAI兼容接口（通过代理）
"""
import os
from openai import OpenAI
from dotenv import load_dotenv
import time
import json

# 加载环境变量
load_dotenv()

# 使用OpenAI兼容接口（通过代理）
client = OpenAI(
    api_key=os.getenv("OPENAI_API_KEY"),
    base_url=os.getenv("OPENAI_BASE_URL")
)

def test_async_embedding_v1():
    """测试 text-embedding-async-v1"""
    print("=" * 60)
    print("测试1: text-embedding-async-v1 (100条)")
    print("=" * 60)
    
    input_texts = [f"这是测试文本第{i}条" for i in range(100)]
    
    start_time = time.time()
    
    try:
        resp = client.embeddings.create(
            model="text-embedding-async-v1",
            input=input_texts
        )
        
        elapsed = time.time() - start_time
        
        print(f"✓ 成功！")
        print(f"  处理文本数: {len(input_texts)}")
        print(f"  返回向量数: {len(resp.data)}")
        print(f"  向量维度: {len(resp.data[0].embedding)}")
        print(f"  耗时: {elapsed:.2f}秒")
        print(f"  速度: {len(input_texts)/elapsed:.1f} texts/秒")
        
    except Exception as e:
        print(f"✗ v1失败: {e[:150]}")
        
        # 尝试v2
        print("\n尝试 text-embedding-async-v2...")
        try:
            resp = client.embeddings.create(
                model="text-embedding-async-v2",
                input=input_texts
            )
            elapsed = time.time() - start_time
            print(f"✓ v2成功！速度: {len(input_texts)/elapsed:.1f} texts/秒")
        except Exception as e2:
            print(f"✗ v2也失败: {str(e2)[:100]}")

def test_async_with_different_sizes():
    """测试async-v1不同批量大小"""
    print("\n" + "=" * 60)
    print("测试2: async-v1 不同批量大小")
    print("=" * 60)
    
    batch_sizes = [10, 50, 100, 500, 1000, 2000]
    
    for size in batch_sizes:
        input_texts = [f"文本{i}" for i in range(size)]
        start_time = time.time()
        
        try:
            resp = client.embeddings.create(
                model="text-embedding-async-v1",
                input=input_texts
            )
            
            elapsed = time.time() - start_time
            speed = size / elapsed
            print(f"  ✓ 批量 {size:4d}: {elapsed:5.2f}秒, {speed:6.1f} texts/秒")
            time.sleep(0.5)
            
        except Exception as e:
            error_msg = str(e)
            if "batch size" in error_msg.lower():
                print(f"  ✗ 批量 {size:4d}: 超过批量限制")
                print(f"\n结论: async-v1 最大批量 < {size}")
                break
            elif "model_not_found" in error_msg or "404" in error_msg:
                print(f"  ✗ async-v1 模型不可用")
                break
            else:
                print(f"  ✗ 批量 {size:4d}: {error_msg[:80]}")
                break

def test_regular_v4_comparison():
    """对比：v4 vs async-v1 处理100条数据"""
    print("\n" + "=" * 60)
    print("测试3: v4 vs async-v1 性能对比 (100条)")
    print("=" * 60)
    
    all_texts = [f"实体描述{i}" for i in range(100)]
    
    # 测试v4（批量10）
    print("\ntext-embedding-v4 (批量10):")
    start_time = time.time()
    batch_size = 10
    
    try:
        for i in range(0, len(all_texts), batch_size):
            batch = all_texts[i:i+batch_size]
            resp = client.embeddings.create(
                model="text-embedding-v4",
                input=batch
            )
            time.sleep(0.2)
        
        v4_time = time.time() - start_time
        v4_speed = len(all_texts) / v4_time
        print(f"  耗时: {v4_time:.2f}秒, 速度: {v4_speed:.1f} texts/秒")
    except Exception as e:
        print(f"  失败: {e}")
        v4_time = None
    
    # 测试async-v1（一次性）
    print("\ntext-embedding-async-v1 (一次性100):")
    start_time = time.time()
    
    try:
        resp = client.embeddings.create(
            model="text-embedding-async-v1",
            input=all_texts
        )
        
        async_time = time.time() - start_time
        async_speed = len(all_texts) / async_time
        print(f"  耗时: {async_time:.2f}秒, 速度: {async_speed:.1f} texts/秒")
        
        if v4_time:
            speedup = v4_time / async_time
            print(f"\n提速: {speedup:.1f}x")
        
    except Exception as e:
        print(f"  失败: {str(e)[:100]}")

if __name__ == "__main__":
    print("text-embedding-async 异步批量测试\n")
    
    test_async_embedding_v1()
    test_async_with_different_sizes()
    test_regular_v4_comparison()
    
    print("\n" + "=" * 60)
    print("测试完成！")
    print("=" * 60)
