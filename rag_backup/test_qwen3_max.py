#!/usr/bin/env python3
"""测试 qwen3-max 模型"""

import os
from openai import OpenAI
from dotenv import load_dotenv

# 禁用代理
for proxy_var in ['HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy', 'ALL_PROXY', 'all_proxy']:
    if proxy_var in os.environ:
        del os.environ[proxy_var]

# 加载 .env 配置
load_dotenv()

# 初始化客户端
client = OpenAI(
    api_key=os.getenv("OPENAI_API_KEY"),
    base_url=os.getenv("OPENAI_BASE_URL")
)

def test_model(model_name: str, test_reasoning: bool = False, enable_thinking: bool = False):
    """测试指定模型"""
    mode_desc = " [思考模式]" if enable_thinking else ""
    print(f"\n{'='*60}")
    print(f"测试模型: {model_name}{mode_desc}")
    print(f"{'='*60}")
    
    try:
        # 选择测试问题
        if test_reasoning:
            # 需要推理的问题
            messages = [
                {"role": "system", "content": "你是一个逻辑推理专家。"},
                {"role": "user", "content": "小明比小红高，小红比小华高。那么小明和小华谁更高？请先分析再回答。"}
            ]
        else:
            # 简单的测试问题
            messages = [
                {"role": "system", "content": "你是一个有帮助的AI助手。"},
                {"role": "user", "content": "请用一句话介绍你自己，并说明你的模型名称。"}
            ]
        
        print("发送请求...")
        
        # 构建请求参数
        request_params = {
            "model": model_name,
            "messages": messages,
            "temperature": 0.7,
            "max_tokens": 2000
        }
        
        # 如果启用思考模式，尝试添加相关参数
        if enable_thinking:
            # 尝试不同的可能参数名
            request_params["extra_body"] = {
                "enable_thinking": True,
                "thinking_mode": True,
                "enable_search": False
            }
        
        response = client.chat.completions.create(**request_params)
        
        # 打印结果
        message = response.choices[0].message
        content = message.content
        
        print(f"\n✓ 请求成功!")
        
        # 检查是否有思考过程
        reasoning_content = getattr(message, 'reasoning_content', None)
        if reasoning_content:
            print(f"\n🧠 思考过程:")
            print(f"{reasoning_content}")
            print(f"\n💬 最终回答:")
        else:
            print(f"💬 模型响应:")
        
        print(f"{content}")
        print(f"\n使用 tokens: {response.usage.total_tokens}")
        print(f"  - Prompt tokens: {response.usage.prompt_tokens}")
        print(f"  - Completion tokens: {response.usage.completion_tokens}")
        
        return True
        
    except Exception as e:
        print(f"\n✗ 请求失败: {e}")
        import traceback
        traceback.print_exc()
        return False

def main():
    print(f"API Base URL: {os.getenv('OPENAI_BASE_URL')}")
    print(f"API Key: {os.getenv('OPENAI_API_KEY')[:20]}...")
    
    # 第一轮：测试 qwen3-max-preview
    print(f"\n{'#'*60}")
    print("# 第一轮：qwen3-max-preview（正式版）")
    print(f"{'#'*60}")
    test_model("qwen3-max-preview", test_reasoning=True, enable_thinking=False)
    
    # 第二轮：测试 qwen3-max-preview 思考模式
    print(f"\n{'#'*60}")
    print("# 第二轮：qwen3-max-preview 思考模式")
    print(f"{'#'*60}")
    test_model("qwen3-max-preview", test_reasoning=True, enable_thinking=True)
    
    # 第三轮：对比 qwen3-max（正式版）
    print(f"\n{'#'*60}")
    print("# 第三轮：qwen3-max（正式版，非Preview）")
    print(f"{'#'*60}")
    test_model("qwen3-max", test_reasoning=True, enable_thinking=False)
    
    # 第四轮：对比 qwen-flash
    print(f"\n{'#'*60}")
    print("# 第四轮：qwen-flash（快速模型）")
    print(f"{'#'*60}")
    test_model("qwen-flash", test_reasoning=True, enable_thinking=False)
    
    # 总结
    print(f"\n{'='*60}")
    print("测试总结:")
    print(f"{'='*60}")
    print(f"  qwen3-max Preview 版本支持:")
    print(f"    - 非思考模式: 常规推理，推理过程在输出中")
    print(f"    - 思考模式: 内置思考能力增强（可能需要特定API参数）")
    print(f"  如果看到 reasoning_content，说明思考模式成功启用")
    print(f"  否则思考过程融合在模型内部，不单独暴露")

if __name__ == "__main__":
    main()
