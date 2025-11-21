#!/usr/bin/env python3
"""
直接测试API调用，不经过任何封装
用于诊断问题
"""

import asyncio
import base64
import os
from openai import AsyncOpenAI
from dotenv import load_dotenv

load_dotenv()

async def test_direct_api():
    """直接测试API"""
    
    # 读取图片
    image_path = "temp_test_images/千味央厨-公司深度分析千滋百味厨师之选-22041226页_page_1.png"
    
    if not os.path.exists(image_path):
        print(f"图片不存在: {image_path}")
        return
    
    with open(image_path, "rb") as f:
        image_base64 = base64.b64encode(f.read()).decode("utf-8")
    
    print(f"图片路径: {image_path}")
    print(f"Base64长度: {len(image_base64)}")
    print(f"Base64前100字符: {image_base64[:100]}")
    
    # 创建客户端
    client = AsyncOpenAI(
        api_key=os.getenv("OPENAI_API_KEY"),
        base_url=os.getenv("OPENAI_BASE_URL")
    )
    
    # 构建消息
    messages = [
        {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": "请描述这张图片的内容"
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": f"data:image/png;base64,{image_base64}"
                    }
                }
            ]
        }
    ]
    
    print("\n调用API...")
    
    try:
        response = await client.chat.completions.create(
            model="qwen-vl-max",
            messages=messages,
            max_tokens=1000
        )
        
        content = response.choices[0].message.content
        print(f"\n=== API响应 ===")
        print(content)
        print(f"\nToken使用: {response.usage.total_tokens}")
        
    except Exception as e:
        print(f"错误: {e}")
        import traceback
        traceback.print_exc()
    
    finally:
        await client.close()

if __name__ == "__main__":
    asyncio.run(test_direct_api())
