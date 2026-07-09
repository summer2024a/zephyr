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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ZEPHYR_PROJECT="/work/zephyr-rtos/zephyrproject"
ZEPHYR_BASE="$ZEPHYR_PROJECT"
BOARD="s905y4_2g"
BUILD_DIR="build_s4_shell"
UIMG_LOCAL="$ZEPHYR_PROJECT/$BUILD_DIR/zephyr/zephyr.uimg"
REMOTE_SCRIPT_DIR="/mnt/49.20/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g"

# 日志文件（85 上，每次 monitor 启动刷新）
LOG_FILE=""
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

# OVERLAY: irq-rx | poll-uart | (empty = defconfig irq-full + SMP)
#   (empty)    — defconfig: Shell TX/RX 全中断 + SMP 4 核
#   irq-rx     — Shell RX 中断 + TX 轮询
#   poll-uart  — Shell 全轮询
resolve_build_cmake_args() {
    local args="${EXTRA_CMAKE_ARGS:-}"
    if [ -n "${OVERLAY:-}" ]; then
        local conf="$SCRIPT_DIR/overlay-${OVERLAY}.conf"
        if [ ! -f "$conf" ]; then
            log_err "未知 OVERLAY=${OVERLAY}，缺少 $conf"
            log_info "可用: irq-rx, poll-uart"
            exit 1
        fi
        log_info "OVERLAY=${OVERLAY} → $conf"
        local overlay_arg="-DEXTRA_CONF_FILE=$conf"
        if [ -n "$args" ]; then
            args="$args $overlay_arg"
        else
            args="$overlay_arg"
        fi
    fi
    echo "$args"
}

build_image() {
    log_info "编译 Zephyr 镜像 ($BOARD)..."
    cd "$ZEPHYR_PROJECT"
    source zephyr/zephyr-env.sh
    local extra_args
    extra_args=$(resolve_build_cmake_args)
    if [ -n "$extra_args" ]; then
        log_info "CMake extra: $extra_args"
        # shellcheck disable=SC2086
        west build -b "$BOARD" -d "$BUILD_DIR" -s zephyr/samples/hello_world --pristine -- \
            $extra_args
    else
        west build -b "$BOARD" -d "$BUILD_DIR" -s zephyr/samples/hello_world --pristine
    fi
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
    local shell_cmds="${ZEPHYR_SHELL_CMDS:-help,kernel uptime,kernel uptime,kernel version,meson_s4_gic}"

    LOG_FILE="/tmp/s4_boot_$(date +%Y%m%d_%H%M%S).log"
    cleanup_monitor

    # 同步监控脚本到串口服务器（85 上路径见 REMOTE_SCRIPT_DIR）
    sshpass -p "$SERIAL_PASS" scp -o StrictHostKeyChecking=no \
        "$SCRIPT_DIR/remote_serial_test.py" \
        "$SERIAL_USER@$SERIAL_HOST:$REMOTE_SCRIPT_DIR/remote_serial_test.py" 2>/dev/null || true

    local no_boot_flag=""
    if [ "$no_boot" = "--no-boot" ]; then
        no_boot_flag="--no-boot"
    fi

    log_info "启动串口监控 (后台): $SERIAL_HOST:$SERIAL_DEV @ $SERIAL_BAUD"
    log_info "日志: $LOG_FILE"

    ssh_serial "echo '$SERIAL_PASS' | sudo -S -p '' bash -c '
        cd \"$REMOTE_SCRIPT_DIR\" || exit 1
        printf \"%s\" \"$shell_cmds\" > /tmp/s4_shell_cmds.txt
        nohup env ZEPHYR_SHELL_CMDS=\"\$(cat /tmp/s4_shell_cmds.txt)\" python3 remote_serial_test.py \
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

    if grep -aqE 'smp: start cpu|smp: secondary|Secondary CPU core' "$log_path"; then
        log_info "SMP trace:"
        grep -aE 'smp: start cpu|smp: secondary|Secondary CPU core' "$log_path" | tail -10
    fi

    # SMP boot trace markers: Q+=9 primary, <>{>~! secondary
    if grep -aqE '[Q+=9<>{}~!]' "$log_path"; then
        local smp_markers
        smp_markers=$(grep -ao 'HGgLP[0-9A-Za-z<>{}~!+=9Q]*' "$log_path" 2>/dev/null | tail -1 || true)
        if [ -n "$smp_markers" ]; then
            log_info "SMP markers tail: ${smp_markers: -40}"
        fi
    fi

    if grep -aq "Secondary CPU core" "$log_path"; then
        log_ok "检测到 SMP secondary CPU 上线"
        grep -a "Secondary CPU core" "$log_path" | tail -3
    fi

    if grep -aqE 'arch_num_cpus\(\)=4|OK: 4 CPUs online|z_smp_init returned, arch_num_cpus\(\)=4' "$log_path"; then
        log_ok "SMP 4 核在线"
        grep -aE 'arch_num_cpus\(\)=4|OK: 4 CPUs online|z_smp_init returned|Secondary CPU core' "$log_path" | tail -5
    elif grep -aq "deferred z_smp_init done" "$log_path"; then
        log_ok "SMP defer init 完成"
        grep -a "deferred z_smp_init" "$log_path" | tail -2
    elif grep -aq "meson_s4_smp" "$log_path"; then
        log_warn "已执行 meson_s4_smp 但未确认 4 核在线"
    fi

    if grep -aqE 'uart:\~\$|uart:~ ' "$log_path"; then
        log_ok "Shell 提示符已出现"
    fi

    if grep -aqE 'Please press the <Tab>|Available commands:' "$log_path"; then
        log_ok "Shell help 有回显"
    fi

    if grep -aqE 'Kernel version:|Zephyr version' "$log_path"; then
        log_ok "Shell kernel version 有回显"
    fi

    # Tick / Arch Timer via kernel uptime (two samples should increase)
    local uptime_vals
    uptime_vals=$(grep -aoE 'Uptime: [0-9]+ ms' "$log_path" 2>/dev/null | grep -oE '[0-9]+' || true)
    if [ -n "$uptime_vals" ]; then
        local u1 u2
        u1=$(echo "$uptime_vals" | sed -n '1p')
        u2=$(echo "$uptime_vals" | sed -n '2p')
        log_info "kernel uptime: 1st=${u1:-?}ms 2nd=${u2:-?}ms"
        if [ -n "$u1" ] && [ -n "$u2" ] && [ "$u2" -gt "$u1" ]; then
            log_ok "Tick 正常增长 (Arch Timer/GIC PPI, +$((u2 - u1)) ms)"
        elif [ -n "$u1" ]; then
            log_warn "仅一次 uptime 或第二次未增长 (GIC/tick 待查)"
        fi
    fi

    if grep -aqE 'meson_uart_isr_count=[1-9]' "$log_path"; then
        log_ok "UART ISR 有触发 (irq-full/irq-rx)"
        grep -a 'meson_uart_isr_count=' "$log_path" | tail -3
    fi

    if grep -aqE 'zephyr_irq=201' "$log_path"; then
        log_ok "meson_s4_gic: SPI 201 诊断已输出"
    fi

    if grep -aqE 'cycles: [0-9]+ hw cycles' "$log_path"; then
        log_ok "kernel cycles 有回显 (hw cycle counter)"
        grep -aoE 'cycles: [0-9]+ hw cycles' "$log_path" | head -1
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
    dual_thread_boot "" "80" "${POST_BOOT_SEC:-55}"
}

stability_boot() {
    local runs="${STABILITY_RUNS:-3}"
    export ZEPHYR_SHELL_CMDS="help,kernel uptime,kernel uptime,kernel version,meson_s4_gic"
    log_info "========== 稳定性测试 (${runs} 次启动, defconfig irq-full+SMP) =========="
    build_image
    deploy_image
    local i pass=0 fail=0
    for i in $(seq 1 "$runs"); do
        log_info "---------- Run $i/$runs ----------"
        dual_thread_boot "" "80" "${POST_BOOT_SEC:-55}" || { fail=$((fail + 1)); continue; }
        local local_log
        local_log=$(fetch_log)
        if [ -n "$local_log" ] && grep -aqE 'OK: 4 CPUs online|arch_num_cpus\(\)=4|z_smp_init returned, arch_num_cpus\(\)=4' "$local_log" \
            && grep -aqE 'uart:\~\$|uart:~ ' "$local_log" \
            && grep -aqE 'Please press the <Tab>|Available commands:' "$local_log"; then
            pass=$((pass + 1))
            log_ok "Run $i: PASS"
        else
            fail=$((fail + 1))
            log_warn "Run $i: FAIL (see $local_log)"
        fi
        [ "$i" -lt "$runs" ] && sleep 3
    done
    log_info "稳定性结果: ${pass}/${runs} PASS, ${fail}/${runs} FAIL"
    [ "$fail" -eq 0 ]
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
    echo "  stability   - irq-full+SMP 连续启动 (STABILITY_RUNS=3 默认)"
    echo "  analyze     - 分析 /tmp/s4_boot_*.log"
    echo "  help        - 显示此帮助"
    echo ""
    echo "Defconfig: Shell irq-full + SMP auto-probe (main z_smp_init) + 4 核"
    echo ""
    echo "Overlay (覆盖 defconfig Shell 模式):"
    echo "  OVERLAY=irq-rx     Shell RX 中断 + TX 轮询"
    echo "  OVERLAY=poll-uart  Shell 全轮询"
    echo "  (不设 OVERLAY)     irq-full (默认)"
    echo ""
    echo "示例:"
    echo "  $0 boot                    # irq-full + 默认稳定性 Shell 命令"
    echo "  $0 stability               # 连续 3 次启动验收"
    echo "  STABILITY_RUNS=5 $0 stability"
    echo "  OVERLAY=poll-uart $0 boot"
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
    stability)
        check_sshpass
        stability_boot
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
