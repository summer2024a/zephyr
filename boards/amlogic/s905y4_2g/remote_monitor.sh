#!/bin/bash
# Amlogic S4 远程监控启动脚本
# 在串口服务器 192.168.53.85 上直接运行，避免 SSH 延迟

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MONITOR_SCRIPT="$SCRIPT_DIR/remote_serial_test.py"
SERIAL_DEV="/dev/ttyUSB0"
BAUD=921600
LOG_FILE="/tmp/s4_boot_$(date +%Y%m%d_%H%M%S).log"
BOOT_WAIT=80
NO_BOOT=""

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --dev) SERIAL_DEV="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --log) LOG_FILE="$2"; shift 2 ;;
        --boot-wait) BOOT_WAIT="$2"; shift 2 ;;
        --no-boot) NO_BOOT="--no-boot"; shift ;;
        --help) echo "Usage: $0 [--dev DEV] [--baud BAUD] [--log FILE] [--boot-wait SEC] [--no-boot]"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# 检查监控脚本
if [[ ! -f "$MONITOR_SCRIPT" ]]; then
    echo "ERROR: Monitor script not found: $MONITOR_SCRIPT"
    exit 1
fi

# 检查串口设备
if [[ ! -e "$SERIAL_DEV" ]]; then
    echo "ERROR: Serial device not found: $SERIAL_DEV"
    exit 1
fi

# 检查 sudo 权限
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: This script must be run with sudo"
    exit 1
fi

# 运行监控脚本
echo "[INFO] Starting monitor on $SERIAL_DEV @ $BAUD baud"
echo "[INFO] Log file: $LOG_FILE"

python3 "$MONITOR_SCRIPT" \
    --dev "$SERIAL_DEV" \
    --baud "$BAUD" \
    --log "$LOG_FILE" \
    --boot-wait "$BOOT_WAIT" \
    $NO_BOOT

# 输出日志位置
echo "[INFO] Log saved to: $LOG_FILE"