#!/bin/bash
# SSH隧道重启脚本

echo "🔄 重启SSH隧道..."

# 杀掉所有dev229-api的SSH连接
pkill -f "ssh -fN dev229-api"
sleep 2

# 重新建立SSH隧道
ssh -fN dev229-api

# 等待1秒
sleep 1

# 检查是否成功
if pgrep -f "ssh -fN dev229-api" > /dev/null; then
    echo "✅ SSH隧道已重启"
    echo "📡 测试API连接..."
    
    # 测试API
    if curl -s --max-time 3 http://127.0.0.1:18081/v1/models > /dev/null 2>&1; then
        echo "✅ API连接正常"
    else
        echo "⚠️  API连接测试超时，可能需要等待几秒"
    fi
else
    echo "❌ SSH隧道启动失败"
    exit 1
fi
