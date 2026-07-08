#!/bin/bash
# Amlogic S4 (S905Y4/S905W2) TX3 mini plus Zephyr 自动化调试脚本
#
# 用法:
#   ./board_test.sh [命令]
#
# 命令:
#   monitor     - 仅监控串口输出，停在 U-Boot
#   reset       - 仅复位设备
#   boot        - 编译 + 部署 + 复位 + 监控完整流程（自动停在 U-Boot 并启动 Zephyr）
#   uboot       - 复位 + 监控，停在 U-Boot 不启动
#   tftp_boot   - 通过 TFTP 启动 Zephyr（手动命令提示）
#   help        - 显示帮助信息
#
# 环境拓扑:
#   [编译机] /work/zephyr-rtos/zephyrproject
#        │  west build → build_s4_shell/zephyr/zephyr.uimg
#        │
#   [142] 192.168.53.142  TFTP server + USB 上下电
#        │  /home/lynxi/usb_power/amlogic_s4_reset.sh
#        │
#   [85]  192.168.53.85   串口 /dev/ttyUSB0 @ 921600
#        └── TX3 mini plus (S905W2, 4×A55)

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
UIMG_LOCAL="build_s4_shell/zephyr/zephyr.uimg"
UIMG_REMOTE="zephyr.uimg"

ZEPHYR_BASE="/work/zephyr-rtos/zephyrproject"
BOARD="s905y4_2g"
BUILD_DIR="build_s4_shell"
SCRIPT_DIR="$ZEPHYR_BASE/zephyr/boards/amlogic/s905y4_2g"

# 日志文件
LOG_FILE="/tmp/s4_boot_$(date +%Y%m%d_%H%M%S).log"
CTL_LOG="/tmp/s4_monitor_ctl.log"

# U-Boot 启动命令
UBOOT_CMDS='setenv serverip 192.168.53.142;setenv ipaddr 192.168.53.130;setenv loadkernel tftpboot 0x01000000 zephyr.uimg;setenv uenvcmd "run loadkernel; bootm 0x01000000"'

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_ok() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_err() { echo -e "${RED}[ERR]${NC} $1"; }

# 检查 sshpass 是否安装
check_sshpass() {
    if ! command -v sshpass &> /dev/null; then
        log_err "sshpass 未安装，请执行: sudo apt install sshpass"
        exit 1
    fi
}

# 编译 Zephyr 镜像
build_image() {
    log_info "编译 Zephyr 镜像 ($BOARD)..."
    cd "$ZEPHYR_BASE"
    source zephyr/zephyr-env.sh
    west build -b $BOARD -d $BUILD_DIR -s zephyr/samples/hello_world --pristine
    log_ok "编译完成: $BUILD_DIR/zephyr/zephyr.uimg"
    ls -lh $BUILD_DIR/zephyr/zephyr.uimg
}

# 部署镜像到 TFTP 服务器
deploy_image() {
    log_info "部署镜像到 TFTP 服务器 ($POWER_HOST)..."
    sshpass -p "$POWER_PASS" ssh -o StrictHostKeyChecking=no $POWER_USER@$POWER_HOST \
        "ls -lh $TFTP_DIR/$UIMG_REMOTE" || log_warn "TFTP 目录中尚未有镜像"
    log_ok "TFTP 服务器已就绪 (镜像已通过 SMB 挂载自动同步)"
}

# 复位设备
reset_device() {
    log_info "复位设备..."
    sshpass -p "$POWER_PASS" ssh -o StrictHostKeyChecking=no $POWER_USER@$POWER_HOST \
        "sudo $RESET_SCRIPT"
    log_ok "设备已复位"
    sleep 2
}

# 监控串口输出（使用 Python 脚本自动停在 U-Boot）
monitor_serial() {
    local duration="${1:-80}"
    local post_boot="${2:-15}"
    local no_boot="${3:-}"
    
    log_info "启动串口监控..."
    log_info "串口: $SERIAL_HOST:$SERIAL_DEV @ $SERIAL_BAUD"
    log_info "等待 U-Boot: ${duration}s, 启动后捕获: ${post_boot}s"
    
    local py_script="$SCRIPT_DIR/remote_serial_test.py"
    local opts=""
    if [ "$no_boot" = "--no-boot" ]; then
        opts="--no-boot"
        log_info "仅停在 U-Boot，不自动启动"
    fi
    
    # 通过 SSH 执行 Python 监控脚本
    # 需要: 1) 脚本已通过 SMB 挂载到 85; 2) sudo 权限
    sshpass -p "$SERIAL_PASS" ssh -t -o StrictHostKeyChecking=no $SERIAL_USER@$SERIAL_HOST \
        "cd /mnt/49.20/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g && \
         echo '$SERIAL_PASS' | sudo -S python3 remote_serial_test.py \
         --dev $SERIAL_DEV --baud $SERIAL_BAUD \
         --log $LOG_FILE --boot-wait $duration --post-boot $post_boot $opts" \
        | tee "$CTL_LOG"
}

# 发送 U-Boot 命令并启动
send_uboot_commands() {
    log_info "发送 U-Boot 启动命令..."
    
    # 使用 expect 或 screen 发送命令
    # 这里需要交互式操作，建议手动执行
    log_warn "U-Boot 命令需要手动输入，请在串口监控中按 Enter 停止 autoboot 后执行:"
    echo ""
    echo "  setenv serverip 192.168.53.142"
    echo "  setenv ipaddr 192.168.53.130"
    echo "  setenv loadkernel tftpboot 0x01000000 zephyr.uimg"
    echo "  setenv uenvcmd \"run loadkernel; bootm 0x01000000\""
    echo "  run uenvcmd"
    echo ""
}

# 双线程架构：线程 a 监控串口，线程 b 复位设备
# 执行顺序：线程 a 先运行 5s 后，再执行线程 b
dual_thread_boot() {
    local no_boot="${1:-}"
    local duration="${2:-80}"
    
    log_info "========== 双线程架构启动 =========="
    log_info "线程 a: 监控串口（先启动）"
    log_info "线程 b: 复位设备（5s 后执行）"
    
    # 1. 清理旧进程和文件
    log_info "清理旧进程和文件..."
    sshpass -p "$SERIAL_PASS" ssh $SERIAL_USER@$SERIAL_HOST \
        "echo '$SERIAL_PASS' | sudo -S pkill -9 -f remote_serial_test.py; rm -rf /tmp/s4_*" 2>/dev/null || true
    sleep 2
    
    # 2. 线程 a: 启动串口监控（不设置 timeout，持续监控）
    log_info "启动线程 a: 串口监控..."
    local py_opts=""
    if [ "$no_boot" = "--no-boot" ]; then
        py_opts="--no-boot"
        log_info "仅停在 U-Boot，不自动启动 Zephyr"
    fi
    
    # 使用 ssh -t 强制分配 pseudo-terminal，确保 SSH 在后台命令启动后返回
    # 关键修复：不使用 nohup/&，使用 && echo MONITOR_STARTED 确保返回标记
    sshpass -p "$SERIAL_PASS" ssh -t $SERIAL_USER@$SERIAL_HOST \
        "cd /mnt/49.20/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g && \
         echo '$SERIAL_PASS' | sudo -S python3 remote_serial_test.py \
         --dev $SERIAL_DEV --baud $SERIAL_BAUD \
         --log $LOG_FILE --boot-wait $duration --ready-file /tmp/s4_monitor_ready.txt $py_opts \
         > /tmp/s4_monitor_stdout.log 2>&1 && echo MONITOR_STARTED" || log_warn "SSH 命令执行完成"
    
    # 3. 等待线程 a 就绪（MONITOR_READY）
    log_info "等待线程 a 就绪..."
    local ready_wait=30
    local waited=0
    while [ $waited -lt $ready_wait ]; do
        if sshpass -p "$SERIAL_PASS" ssh $SERIAL_USER@$SERIAL_HOST \
            "test -f /tmp/s4_monitor_ready.txt && cat /tmp/s4_monitor_ready.txt" 2>/dev/null | grep -q "MONITOR_READY"; then
            log_ok "线程 a 已就绪"
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done
    
    if [ $waited -ge $ready_wait ]; then
        log_err "线程 a 就绪超时"
        exit 1
    fi
    
    # 4. 等待 5 秒（用户要求：线程 a 先运行 5s）
    log_info "线程 a 运行 3s..."
    sleep 3
    
    # 5. 线程 b: 执行复位
    log_info "启动线程 b: 复位设备..."
    sshpass -p "$POWER_PASS" ssh $POWER_USER@$POWER_HOST \
        "echo '$POWER_PASS' | sudo -S $RESET_SCRIPT"
    log_ok "设备已复位"
    
    # 6. 等待线程 a 完成
    log_info "等待线程 a 完成（最多 ${duration}s）..."
    sleep $duration
    
    # 7. 终止监控脚本并发送 SIGTERM（确保日志写入）
    log_info "终止监控脚本..."
    sshpass -p "$SERIAL_PASS" ssh $SERIAL_USER@$SERIAL_HOST \
        "echo '$SERIAL_PASS' | sudo -S pkill -TERM -f remote_serial_test.py" 2>/dev/null || true
    sleep 2
    
    # 8. 检查结果
    log_info "检查结果..."
    sshpass -p "$SERIAL_PASS" ssh $SERIAL_USER@$SERIAL_HOST \
        "ls -lh $LOG_FILE 2>/dev/null || echo '日志文件不存在'; \
         grep -iE '(KEYBOX|FAT12|ap201)' $LOG_FILE 2>/dev/null | head -20 || echo '无关键标记'"
    
    if sshpass -p "$SERIAL_PASS" ssh $SERIAL_USER@$SERIAL_HOST \
        "grep -q 'ap201#' $LOG_FILE" 2>/dev/null; then
        log_ok "成功停在 U-Boot (ap201#)"
    else
        log_warn "未停在 U-Boot，可能进入了 Android"
    fi
}

# 完整调试流程: 编译 + 部署 + 双线程启动
full_boot() {
    log_info "========== 完整调试流程 =========="
    
    # 1. 编译
    build_image
    
    # 2. 部署确认
    deploy_image
    
    # 3. 双线程启动
    dual_thread_boot "" "80"
}

# 仅停在 U-Boot（不启动）- 使用双线程架构
stop_at_uboot() {
    log_info "========== 停在 U-Boot（双线程架构）=========="
    dual_thread_boot "--no-boot" "80"
}

# 仅编译和部署
build_and_deploy() {
    build_image
    deploy_image
}

# 显示帮助
show_help() {
    echo "Amlogic S4 (S905Y4/S905W2) TX3 mini plus Zephyr 自动化调试脚本"
    echo ""
    echo "用法: $0 [命令]"
    echo ""
    echo "命令:"
    echo "  build       - 编译 Zephyr 镜像"
    echo "  deploy      - 确认 TFTP 服务器镜像"
    echo "  reset       - 复位设备"
    echo "  monitor     - 监控串口，自动停在 U-Boot 并启动 Zephyr"
    echo "  uboot       - 复位 + 监控，停在 U-Boot 不启动（可手动输入命令）"
    echo "  boot        - 完整流程: 编译 + 部署 + 复位 + 自动停在 U-Boot + 启动 Zephyr"
    echo "  tftp        - 显示 TFTP 启动命令"
    echo "  help        - 显示此帮助"
    echo ""
    echo "环境拓扑:"
    echo "  串口服务器: $SERIAL_USER@$SERIAL_HOST ($SERIAL_DEV @ $SERIAL_BAUD)"
    echo "  供电/TFTP:  $POWER_USER@$POWER_HOST"
    echo "  复位脚本:   $RESET_SCRIPT"
    echo "  TFTP 目录:  $TFTP_DIR"
    echo "  SMB 挂载:   /mnt/49.20/zephyr-rtos/zephyrproject"
    echo ""
    echo "板级 marker:"
    echo "  U-Boot prompt:  s4_ap201#"
    echo "  STOP_MARK:      KEYBOX PART"
    echo "  AUTOBOOT_MARK:  Hit any key to stop autoboot"
    echo "  bootdelay:      1"
    echo ""
    echo "示例:"
    echo "  $0 build              # 编译镜像"
    echo "  $0 reset               # 复位设备"
    echo "  $0 uboot               # 停在 U-Boot（可手动输入命令）"
    echo "  $0 boot                # 完整调试流程，自动启动 Zephyr"
}

# 主入口
case "$1" in
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
        monitor_serial "${2:-80}" "${3:-15}"
        ;;
    uboot)
        check_sshpass
        stop_at_uboot
        ;;
    boot)
        check_sshpass
        full_boot
        ;;
    tftp)
        send_uboot_commands
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        show_help
        exit 1
        ;;
esac