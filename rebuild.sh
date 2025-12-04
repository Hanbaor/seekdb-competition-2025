#!/bin/bash
set -e  # 遇到错误立即停止

echo "========================================"
echo "🚀 开始重新编译 OceanBase (Release)..."
echo "========================================"

# 1. 编译
bash build.sh release -DOB_USE_CCACHE=ON --init --make -j6

echo ""
echo "========================================"
echo "✅ 编译成功！正在停止集群..."
echo "========================================"

# 2. 停止集群
./tools/deploy/obd.sh stop -n obcluster

echo ""
echo "========================================"
echo "📦 正在替换 observer 二进制文件..."
echo "========================================"

# 3. 替换 binary
cp /home/tian/seekdb/build_release/src/observer/observer /data/obcluster/bin/observer
chmod +x /data/obcluster/bin/observer

echo ""
echo "========================================"
echo "🚀 正在启动集群..."
echo "========================================"

# 4. 启动集群
./tools/deploy/obd.sh start -n obcluster

echo ""
echo "========================================"
echo "🎉 全部完成！"
echo "========================================"

