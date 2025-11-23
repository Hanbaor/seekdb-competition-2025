#!/bin/bash

# 检查最近的剪枝验证日志（不需要实时监控）

# 日志文件路径
LOG_FILE="/data/obcluster/log/observer.log"

echo "========================================"
echo "检查最近的剪枝验证日志"
echo "日志路径: $LOG_FILE"
echo "========================================"
echo ""

if [ ! -f "$LOG_FILE" ]; then
    echo "警告: 日志文件不存在！"
    echo "尝试查找可能的 observer.log..."
    FOUND_LOG=$(find /home/tian -name "observer.log" -type f 2>/dev/null | head -1)
    if [ -n "$FOUND_LOG" ]; then
        echo "找到替代日志: $FOUND_LOG"
        LOG_FILE="$FOUND_LOG"
    else
        echo "错误: 未找到任何 observer.log 文件"
        exit 1
    fi
fi

echo "1. 检查 function lookup 是否被禁用："
echo "----------------------------------------"
grep "SR PRUNING VERIFICATION" "$LOG_FILE" | tail -5
echo ""

echo "2. 检查设置的 default range（docid 上界）："
echo "----------------------------------------"
grep "SR PRUNING: default range" "$LOG_FILE" | tail -10
echo ""

echo "3. 检查倒排索引实际扫描的 docid 范围："
echo "----------------------------------------"
grep "SR PRUNING: inv scan returned" "$LOG_FILE" | tail -20
echo ""

echo "4. 检查最终缓存的结果 docid："
echo "----------------------------------------"
grep "SR PRUNING: cache result" "$LOG_FILE" | tail -20
echo ""

echo "========================================"
echo "统计信息："
echo "----------------------------------------"
echo "Function lookup 禁用次数: $(grep -c "SR PRUNING VERIFICATION" "$LOG_FILE")"
echo "Default range 创建次数: $(grep -c "SR PRUNING: default range" "$LOG_FILE")"
echo "倒排扫描批次数: $(grep -c "SR PRUNING: inv scan returned" "$LOG_FILE")"
echo "缓存结果数: $(grep -c "SR PRUNING: cache result" "$LOG_FILE")"
echo "========================================"
