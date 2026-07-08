#!/bin/bash
# Amlogic S4 (S905Y4/S905W2) TX3 mini plus Zephyr 自动化调试脚本
#
# 用法:
#   ./board_test.sh [命令]
#
# 命令:
#   build       - 编译 Zephyr 镜像
#   deploy      - 确认 TFTP 服务器镜像
#   reset       - 仅复位设备
#   monitor     - 监控串口（可选 post-boot 秒数）
#   uboot       - 复位 + 监控，停在 U-Boot
#   boot        - 编译 + 部署 + 复位 + TFTP 启动 + 分析日志
#   analyze     - 分析最近一次日志
#   help        - 显示帮助
#
# 环境说明见 Test_env.md，详细文档见 doc/

set -e

# 配置
SERIAL_HOST="192.168.53.85"
SERIAL_USER="lynxi"
SERIAL_PASS="Lynxi#123+"
SERIAL_DEV="/dev/ttyUSB0"
SERIAL_BAUD="921600"

POWER_HOST="192.168.53.142"
POWER_USER="lynxi"
POWER_PASS="Lynxi#123+"
RESET_SCRIPT="/home/lynxi/usb_power/amlogic_s4_reset.sh"

TFTP_DIR="/data/work/tftpboot"
UIMG_REMOTE="zephyr.uimg"

SCRIPT_DIR="$ZEPHYR_PROJECT/zephyr/boards/amlogic/s905y4_2g"

ZEPHYR_PROJECT="/work/zephyr-rtos/zephyrproject"
ZEPHYR_BASE="$ZEPHYR_PROJECT"
BOARD="s905y4_2g"
BUILD_DIR="build_s4_shell"
UIMG_LOCAL="$ZEPHYR_PROJECT/$BUILD_DIR/zephyr/zephyr.uimg"
REMOTE_SCRIPT_DIR="/mnt/49.20/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g"

# 日志文件（85 上）
LOG_FILE="/tmp/s4_boot_$(date +%Y%m%d_%H%M%S).log"
CTL_LOG="/tmp/s4_monitor_ctl.log"
READY_FILE="/tmp/s4_monitor_ready.txt"
MONITOR_STDOUT="/tmp/s4_monitor_stdout.log"
MONITOR_PID_FILE="/tmp/s4_monitor.pid"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_ok() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_err() { echo -e "${RED}[ERR]${NC} $1"; }

ssh_serial() {
    sshpass -p "$SERIAL_PASS" ssh -o StrictHostKeyChecking=no "$SERIAL_USER@$SERIAL_HOST" "$@"
}

ssh_power() {
    sshpass -p "$POWER_PASS" ssh -o StrictHostKeyChecking=no "$POWER_USER@$POWER_HOST" "$@"
}

check_sshpass() {
    if ! command -v sshpass &> /dev/null; then
        log_err "sshpass 未安装，请执行: sudo apt install sshpass"
        exit 1
    fi
}

build_image() {
    log_info "编译 Zephyr 镜像 ($BOARD)..."
    cd "$ZEPHYR_PROJECT"
    source zephyr/zephyr-env.sh
    west build -b "$BOARD" -d "$BUILD_DIR" -s zephyr/samples/hello_world --pristine
    log_ok "编译完成: $UIMG_LOCAL"
    ls -lh "$UIMG_LOCAL"
}

deploy_image() {
    log_info "部署镜像到 TFTP 服务器 ($POWER_HOST)..."
    sshpass -p "$POWER_PASS" scp -o StrictHostKeyChecking=no \
        "$UIMG_LOCAL" "$POWER_USER@$POWER_HOST:$TFTP_DIR/$UIMG_REMOTE"
    ssh_power "ls -lh $TFTP_DIR/$UIMG_REMOTE"
    log_ok "TFTP 镜像已部署"
}

reset_device() {
    log_info "复位设备..."
    ssh_power "echo '$POWER_PASS' | sudo -S $RESET_SCRIPT"
    log_ok "设备已复位"
    sleep 2
}

cleanup_monitor() {
    ssh_serial "echo '$SERIAL_PASS' | sudo -S pkill -TERM -f remote_serial_test.py" 2>/dev/null || true
    sleep 1
    ssh_serial "echo '$SERIAL_PASS' | sudo -S pkill -9 -f remote_serial_test.py" 2>/dev/null || true
    ssh_serial "rm -f $READY_FILE $MONITOR_PID_FILE $MONITOR_STDOUT" 2>/dev/null || true
}

start_monitor_bg() {
    local duration="${1:-80}"
    local post_boot="${2:-20}"
    local no_boot="${3:-}"

    cleanup_monitor

    local no_boot_flag=""
    if [ "$no_boot" = "--no-boot" ]; then
        no_boot_flag="--no-boot"
    fi

    log_info "启动串口监控 (后台): $SERIAL_HOST:$SERIAL_DEV @ $SERIAL_BAUD"
    log_info "日志: $LOG_FILE"

    ssh_serial "echo '$SERIAL_PASS' | sudo -S -p '' bash -c '
        cd \"$REMOTE_SCRIPT_DIR\" || exit 1
        nohup python3 remote_serial_test.py \
            --dev \"$SERIAL_DEV\" --baud $SERIAL_BAUD \
            --log \"$LOG_FILE\" \
            --boot-wait $duration --post-boot $post_boot \
            --ready-file \"$READY_FILE\" $no_boot_flag \
            > \"$MONITOR_STDOUT\" 2>&1 &
        echo \$! > \"$MONITOR_PID_FILE\"
    '"

    local waited=0
    while [ $waited -lt 30 ]; do
        if ssh_serial "test -f $READY_FILE && grep -q MONITOR_READY $READY_FILE" 2>/dev/null; then
            log_ok "串口监控已就绪"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done

    log_err "串口监控就绪超时"
    ssh_serial "tail -20 $MONITOR_STDOUT" 2>/dev/null || true
    return 1
}

stop_monitor() {
    log_info "停止串口监控..."
    ssh_serial "echo '$SERIAL_PASS' | sudo -S pkill -TERM -f remote_serial_test.py" 2>/dev/null || true
    sleep 2
}

fetch_log() {
    local local_log="/tmp/$(basename "$LOG_FILE")"
    if ssh_serial "test -f $LOG_FILE"; then
        sshpass -p "$SERIAL_PASS" scp -o StrictHostKeyChecking=no \
            "$SERIAL_USER@$SERIAL_HOST:$LOG_FILE" "$local_log" 2>/dev/null || true
        echo "$local_log"
    else
        echo ""
    fi
}

analyze_log() {
    local log_path="${1:-}"
    if [ -z "$log_path" ]; then
        log_path=$(ls -t /tmp/s4_boot_*.log 2>/dev/null | head -1)
    fi
    if [ -z "$log_path" ] || [ ! -f "$log_path" ]; then
        log_warn "未找到日志文件"
        return 1
    fi

    log_info "分析日志: $log_path ($(wc -c < "$log_path") bytes)"

    if grep -aq "ap201#" "$log_path"; then
        log_ok "检测到 U-Boot prompt (ap201#)"
    else
        log_warn "未检测到 U-Boot prompt"
    fi

    if grep -aq "Bytes transferred" "$log_path"; then
        log_ok "TFTP 传输成功"
        grep -a "Bytes transferred" "$log_path" | tail -1
    fi

    if grep -aq "Zephyr SPL (S4)" "$log_path"; then
        log_ok "检测到 Zephyr SPL 启动"
    fi

    # 提取 boot marker 序列
    local markers
    markers=$(grep -ao 'HGgLP[0-9A-Za-z]*' "$log_path" 2>/dev/null | head -1 || true)
    if [ -n "$markers" ]; then
        log_info "Boot markers: $markers"
    fi

    if grep -aqE 'uart:~|Hello World!|Hello world!' "$log_path"; then
        log_ok "Zephyr 应用已启动"
    fi

    if grep -aq "Starting kernel" "$log_path"; then
        log_warn "检测到 Android 启动 (Starting kernel)"
    fi

    echo ""
    log_info "关键行:"
    grep -aiE '(KEYBOX|FAT12|ap201#|Bytes transferred|Zephyr SPL|Hello World|uart:~|Starting kernel|ESR=)' \
        "$log_path" 2>/dev/null | head -30 || true
}

# 双线程：监控先启动，3s 后复位，等待捕获完成
dual_thread_boot() {
    local no_boot="${1:-}"
    local duration="${2:-80}"
    local post_boot="${3:-20}"

    log_info "========== 双线程启动 =========="
    log_info "线程 a: 串口监控 (85)"
    log_info "线程 b: 设备复位 (142, 监控就绪后 3s)"

    start_monitor_bg "$duration" "$post_boot" "$no_boot" || exit 1

    log_info "监控运行 3s 后复位..."
    sleep 3
    reset_device

    log_info "等待捕获完成 (最多 $((duration + post_boot))s)..."
    local total_wait=$((duration + post_boot + 10))
    local waited=0
    while [ $waited -lt $total_wait ]; do
        if ssh_serial "grep -q CAPTURE_DONE $MONITOR_STDOUT" 2>/dev/null; then
            log_ok "捕获完成"
            break
        fi
        sleep 2
        waited=$((waited + 2))
    done

    stop_monitor

    ssh_serial "tail -5 $MONITOR_STDOUT" 2>/dev/null || true

    local local_log
    local_log=$(fetch_log)
    if [ -n "$local_log" ]; then
        analyze_log "$local_log"
    else
        log_warn "未能获取远程日志"
        ssh_serial "ls -lh $LOG_FILE $MONITOR_STDOUT 2>/dev/null" || true
    fi
}

full_boot() {
    log_info "========== 完整调试流程 =========="
    build_image
    deploy_image
    dual_thread_boot "" "80" "30"
}

stop_at_uboot() {
    log_info "========== 停在 U-Boot =========="
    dual_thread_boot "--no-boot" "80" "5"
}

show_help() {
    echo "Amlogic S4 (S905Y4/S905W2) TX3 mini plus Zephyr 自动化调试脚本"
    echo ""
    echo "用法: $0 [命令]"
    echo ""
    echo "命令:"
    echo "  build       - 编译 Zephyr 镜像"
    echo "  deploy      - 确认/部署 TFTP 镜像"
    echo "  reset       - 复位设备"
    echo "  monitor     - 启动串口监控 (参数: boot_wait post_boot)"
    echo "  uboot       - 复位 + 监控，停在 U-Boot"
    echo "  boot        - 完整流程: 编译 + 部署 + TFTP 启动 + 分析"
    echo "  analyze     - 分析 /tmp/s4_boot_*.log"
    echo "  help        - 显示此帮助"
    echo ""
    echo "文档:"
    echo "  Test_env.md  - 测试环境"
    echo "  doc/         - 调试/启动/移植详细文档"
    echo "  BOOT.md      - 镜像打包与启动"
    echo ""
    echo "环境:"
    echo "  串口: $SERIAL_USER@$SERIAL_HOST ($SERIAL_DEV @ $SERIAL_BAUD)"
    echo "  供电: $POWER_USER@$POWER_HOST"
    echo "  TFTP: $TFTP_DIR/$UIMG_REMOTE"
}

case "${1:-help}" in
    build)
        check_sshpass
        build_image
        ;;
    deploy)
        check_sshpass
        deploy_image
        ;;
    reset)
        check_sshpass
        reset_device
        ;;
    monitor)
        check_sshpass
        dual_thread_boot "" "${2:-80}" "${3:-20}"
        ;;
    uboot)
        check_sshpass
        stop_at_uboot
        ;;
    boot)
        check_sshpass
        full_boot
        ;;
    analyze)
        analyze_log "${2:-}"
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        show_help
        exit 1
        ;;
esac
