#!/bin/bash

LOG_FILE="/data/obcluster/log/observer.log"

echo "========================================"
echo "全文检索剪枝验证日志监控 (Revised)"
echo "========================================"
echo "监控日志: $LOG_FILE"
echo "等待日志输出..."

# 使用 grep --line-buffered 确保实时输出
# 过滤掉无用的干扰信息，只保留 SR PRUNING 相关
tail -f "$LOG_FILE" | grep --line-buffered "SR PRUNING" | while read -r line; do
    # 提取并美化输出
    echo "$line" | sed 's/.*WDIAG.*] //'
done
