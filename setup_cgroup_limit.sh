#!/bin/bash
# 设置 observer 进程的内存限制为 11GB，禁用 swap，并动态监测

LIMIT_GB=11
LIMIT_BYTES=$((LIMIT_GB * 1024 * 1024 * 1024))
CGROUP_PATH="/sys/fs/cgroup/observer_limit"
INTERVAL=0.5  # 监测间隔（秒）

setup_cgroup() {
  echo "=== 设置 Observer 内存限制: ${LIMIT_GB}GB ==="
  
  # 创建 cgroup 目录
  sudo mkdir -p $CGROUP_PATH 2>/dev/null
  
  # 启用 memory 控制器
  echo "+memory" | sudo tee /sys/fs/cgroup/cgroup.subtree_control >/dev/null 2>&1
  
  # 设置内存限制
  echo $LIMIT_BYTES | sudo tee $CGROUP_PATH/memory.max >/dev/null
  
  # 禁用 swap
  echo 0 | sudo tee $CGROUP_PATH/memory.swap.max >/dev/null
}

add_process() {
  OBSERVER_PID=$(pgrep -f "observer.*obcluster")
  if [ -n "$OBSERVER_PID" ]; then
    CURRENT_PID=$(cat $CGROUP_PATH/cgroup.procs 2>/dev/null | head -1)
    if [ "$CURRENT_PID" != "$OBSERVER_PID" ]; then
      echo $OBSERVER_PID | sudo tee $CGROUP_PATH/cgroup.procs >/dev/null
      echo "[$(date +%H:%M:%S)] Observer PID $OBSERVER_PID 已加入 cgroup"
    fi
    echo $OBSERVER_PID
  fi
}

monitor() {
  echo "=== 开始动态监测 (Ctrl+C 退出) ==="
  echo "限制: ${LIMIT_GB}GB | Swap: 禁用"
  echo "-------------------------------------------"
  printf "%-10s %-12s %-12s %-8s\n" "时间" "当前内存" "峰值内存" "使用率"
  echo "-------------------------------------------"
  
  MAX_SEEN=0
  
  while true; do
    # 检查并添加进程
    PID=$(add_process)
    
    if [ -z "$PID" ]; then
      echo "[$(date +%H:%M:%S)] Observer 未运行，等待..."
      sleep 2
      continue
    fi
    
    # 读取 cgroup 内存
    CURRENT=$(cat $CGROUP_PATH/memory.current 2>/dev/null || echo 0)
    
    # 也读取 VmRSS 作为参考
    VMRSS=$(cat /proc/$PID/status 2>/dev/null | grep VmRSS | awk '{print $2}')
    VMRSS_BYTES=$((VMRSS * 1024))
    
    # 更新峰值
    if [ "$VMRSS_BYTES" -gt "$MAX_SEEN" ]; then
      MAX_SEEN=$VMRSS_BYTES
    fi
    
    # 计算使用率
    USAGE_PCT=$(echo "scale=1; $VMRSS_BYTES * 100 / $LIMIT_BYTES" | bc)
    
    # 格式化输出
    CURRENT_GB=$(echo "scale=2; $VMRSS_BYTES / 1024 / 1024 / 1024" | bc)
    MAX_GB=$(echo "scale=2; $MAX_SEEN / 1024 / 1024 / 1024" | bc)
    
    # 超限警告
    if [ $(echo "$VMRSS_BYTES > $LIMIT_BYTES" | bc) -eq 1 ]; then
      printf "\033[31m%-10s %-12s %-12s %-8s ⚠️ 超限!\033[0m\n" \
        "$(date +%H:%M:%S)" "${CURRENT_GB}GB" "${MAX_GB}GB" "${USAGE_PCT}%"
    else
      printf "%-10s %-12s %-12s %-8s\n" \
        "$(date +%H:%M:%S)" "${CURRENT_GB}GB" "${MAX_GB}GB" "${USAGE_PCT}%"
    fi
    
    sleep $INTERVAL
  done
}

# 主流程
setup_cgroup
monitor
