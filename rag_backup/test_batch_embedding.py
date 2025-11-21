"""
测试批量 Embedding API (通过OpenAI兼容接口)
"""
import os
from openai import OpenAI
from dotenv import load_dotenv
import time

# 加载环境变量
load_dotenv()

# 使用OpenAI兼容接口
client = OpenAI(
    api_key=os.getenv("OPENAI_API_KEY"),
    base_url=os.getenv("OPENAI_BASE_URL")
)

def test_single_embedding():
    """测试单条文本embedding"""
    print("=" * 60)
    print("测试1: 单条文本 embedding")
    print("=" * 60)
    
    input_text = "衣服的质量杠杠的，很漂亮，不枉我等了这么久啊"
    
    try:
        resp = client.embeddings.create(
            model="text-embedding-v4",
            input=input_text
        )
        
        print(f"✓ 成功！")
        print(f"  向量维度: {len(resp.data[0].embedding)}")
        print(f"  Token消耗: {resp.usage}")
    except Exception as e:
        print(f"✗ 失败: {e}")

def test_batch_embedding():
    """测试批量文本embedding（同步）"""
    print("\n" + "=" * 60)
    print("测试2: 批量文本 embedding (同步)")
    print("=" * 60)
    
    input_texts = [
        "千味央厨是速冻食品龙头企业",
        "公司主营业务为速冻米面制品",
        "2024年营收增长显著",
        "产品主要供应餐饮渠道",
        "研发投入持续增加"
    ]
    
    start_time = time.time()
    
    try:
        resp = client.embeddings.create(
            model="text-embedding-v4",
            input=input_texts  # 传入列表
        )
        
        elapsed = time.time() - start_time
        
        print(f"✓ 成功！")
        print(f"  处理文本数: {len(input_texts)}")
        print(f"  返回向量数: {len(resp.data)}")
        print(f"  向量维度: {len(resp.data[0].embedding)}")
        print(f"  Token消耗: {resp.usage}")
        print(f"  耗时: {elapsed:.2f}秒")
        print(f"  平均速度: {len(input_texts)/elapsed:.1f} texts/秒")
    except Exception as e:
        print(f"✗ 失败: {e}")

def test_async_batch_embedding():
    """测试大批量embedding性能"""
    print("\n" + "=" * 60)
    print("测试3: 大批量 embedding (100条)")
    print("=" * 60)
    
    # 生成大批量测试数据
    input_texts = [f"这是测试文本第{i}条" for i in range(100)]
    
    start_time = time.time()
    
    try:
        resp = client.embeddings.create(
            model="text-embedding-v4",
            input=input_texts
        )
        
        elapsed = time.time() - start_time
        
        print(f"✓ 成功！")
        print(f"  处理文本数: {len(input_texts)}")
        print(f"  返回向量数: {len(resp.data)}")
        print(f"  Token消耗: {resp.usage}")
        print(f"  耗时: {elapsed:.2f}秒")
        print(f"  平均速度: {len(input_texts)/elapsed:.1f} texts/秒")
        
    except Exception as e:
        print(f"✗ 失败: {e}")

def test_large_batch():
    """测试不同批量大小的性能，找到真实限制"""
    print("\n" + "=" * 60)
    print("测试4: 寻找批量大小限制")
    print("=" * 60)
    
    # 逐步测试批量大小
    batch_sizes = [1, 5, 10, 15, 20, 25, 30, 50, 100, 200, 500]
    
    max_success = 0
    
    for batch_size in batch_sizes:
        input_texts = [f"实体描述文本{i}" for i in range(batch_size)]
        
        start_time = time.time()
        
        try:
            resp = client.embeddings.create(
                model="text-embedding-v4",
                input=input_texts
            )
            
            elapsed = time.time() - start_time
            speed = batch_size / elapsed
            max_success = batch_size
            print(f"  ✓ 批量 {batch_size:3d}: {elapsed:.2f}秒, {speed:4.1f} texts/秒, Token={resp.usage.total_tokens}")
            
            # 等待一下避免限流
            time.sleep(0.5)
                
        except Exception as e:
            error_msg = str(e)
            if "batch size is invalid" in error_msg:
                print(f"  ✗ 批量 {batch_size:3d}: 超过批量限制")
                print(f"\n结论: 最大批量大小 = {max_success}")
                break
            elif "429" in error_msg:
                print(f"  ⚠ 批量 {batch_size:3d}: 触发限流，等待重试...")
                time.sleep(2)
            else:
                print(f"  ✗ 批量 {batch_size:3d}: {error_msg[:100]}")
                break

if __name__ == "__main__":
    print("DashScope 批量 Embedding API 测试\n")
    
    test_single_embedding()
    test_batch_embedding()
    test_async_batch_embedding()
    test_large_batch()
    
    print("\n" + "=" * 60)
    print("测试完成！")
    print("=" * 60)
