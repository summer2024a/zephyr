#!/usr/bin/env python3
"""
Amlogic S4 TX3 mini plus 串口监控脚本

自动停在 U-Boot 并执行启动命令，捕获启动日志。

用法:
    sudo ./remote_serial_test.py [选项]

选项:
    --dev DEVICE      串口设备 (默认 /dev/ttyUSB0)
    --baud BAUD       波特率 (默认 921600)
    --log FILE        日志文件 (默认 /tmp/s4_boot.log)
    --boot-wait SEC   等待 U-Boot 的秒数 (默认 80)
    --post-boot SEC   启动后捕获秒数 (默认 15)
    --no-boot         仅停在 U-Boot，不执行启动命令
    --help            显示帮助

控制输出 (stdout):
    MONITOR_READY     监控就绪，可执行复位
    CAPTURE_DONE status=ok|no_uboot|android bytes=N  捕获完成

环境变量:
    DEV, BAUD, LOG, BOOT_WAIT_SEC, POST_BOOT_SEC
"""

import os
import sys
import select
import termios
import tty
import time
import argparse
import signal
from pathlib import Path

# Amlogic S4 板级配置
UBOOT_PROMPT = "ap201#"  # 使用更宽松的匹配（完整 prompt 是 s4_ap201#）
STOP_MARK = "KEYBOX PART"  # 检测到此标记需要高频发送 Enter
AUTOBOOT_MARK = "Hit any key to stop autoboot"
FAIL_MARK = "Starting kernel"  # Android 启动，退出监控
ZEPHYR_SHELL_MARK = "uart:~"  # Shell 提示符
ZEPHYR_APP_MARK = "Hello World!"  # hello_world 输出

# Shell 验收命令（uart:~ 出现后发送；可用环境变量 ZEPHYR_SHELL_CMDS 覆盖，逗号分隔）
ZEPHYR_SHELL_CMDS = [
    c.strip()
    for c in os.getenv(
        "ZEPHYR_SHELL_CMDS",
        "help,kernel uptime,kernel uptime,kernel version,meson_s4_gic",
    ).split(",")
    if c.strip()
]

# U-Boot 启动命令（TFTP 加载 zephyr.uimg）
UBOOT_BOOT_CMDS = [
    "setenv serverip 192.168.53.142",
    "setenv ipaddr 192.168.53.130",
    "setenv loadkernel tftpboot 0x01000000 zephyr.uimg",
    'setenv uenvcmd "run loadkernel; bootm 0x01000000"',
    "run uenvcmd",
]

def log_msg(msg):
    """调试日志输出"""
    print(msg, file=sys.stderr)
    sys.stderr.flush()

class SerialMonitor:
    def __init__(self, dev, baud, log_file, boot_wait, post_boot, do_boot=True, ready_file="/tmp/s4_monitor_ready"):
        self.dev = dev
        self.baud = baud
        self.log_file = log_file
        self.boot_wait = boot_wait
        self.post_boot = post_boot
        self.do_boot = do_boot
        self.ready_file = ready_file
        
        self.fd = None
        self.buf = bytearray(32768)  # ring buffer
        self.buf_len = 0
        self.log_data = bytearray()
        self.log_fp = None  # 实时日志文件句柄
        
        self.stop_mark_seen = False
        self.uboot_ready = False
        self.boot_started = False
        self.shell_cmds_sent = False
        self._shell_cmds_sent_time = None
        self._app_seen_time = None
        self.status = "no_uboot"
        
    def open_serial(self):
        """打开串口设备并配置"""
        self.fd = os.open(self.dev, os.O_RDWR | os.O_NONBLOCK)
        
        # 配置串口参数
        attrs = termios.tcgetattr(self.fd)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK 
                      | termios.ISTRIP | termios.INLCR | termios.IGNCR 
                      | termios.ICRNL | termios.IXON)
        attrs[1] &= ~(termios.OPOST)
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8
        attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON 
                      | termios.ISIG | termios.IEXTEN)
        attrs[4] = termios.B921600  # baud rate
        attrs[5] = termios.B921600
        
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        # 只清空输出缓冲区，保留输入数据（捕捉 BL1/BL2 早期输出）
        termios.tcflush(self.fd, termios.TCOFLUSH)
        
    def close_serial(self):
        """关闭串口"""
        if self.fd:
            os.close(self.fd)
            self.fd = None
            
    def send_enter(self):
        """发送 Enter 键"""
        os.write(self.fd, b"\r")
        
    def send_line(self, line, newline="\r"):
        """发送一行命令"""
        os.write(self.fd, (line + newline).encode())
        time.sleep(0.3)

    def send_shell_cmd(self, line):
        """Shell 命令：CR+LF，便于中断 RX 路径识别行结束"""
        self.send_line(line, newline="\r\n")
        
    def check_markers(self):
        """检查缓冲区中的标记"""
        data = bytes(self.buf[:self.buf_len])
        
        if FAIL_MARK.encode() in data:
            self.status = "android"
            return True
            
        # 使用更宽松的匹配（ap201# 而不是 s4_ap201#）
        if "ap201#" in data.decode(errors='ignore'):
            self.uboot_ready = True
            self.status = "ok"
            return True
            
        # KEYBOX PART 检测 - 需要持续发送 Enter
        if STOP_MARK.encode() in data:
            self.stop_mark_seen = True
            # 不只发送一次，而是标记需要持续发送
            
        if AUTOBOOT_MARK.encode() in data:
            # bootdelay=1，需要持续发送 Enter
            self.send_enter()
            
        return False

    def _maybe_send_shell_cmds(self, data_str):
        """Send SMP shell commands once uart:~ or timeout after Hello World."""
        if not (self.boot_started and not self.shell_cmds_sent and self.do_boot):
            return

        if ZEPHYR_APP_MARK in data_str and self._app_seen_time is None:
            self._app_seen_time = time.time()
            log_msg("[MONITOR] Hello World seen, waiting for uart:~")

        shell_ready = ZEPHYR_SHELL_MARK in data_str
        app_timeout = (
            self._app_seen_time is not None
            and time.time() - self._app_seen_time > 12
        )
        if not (shell_ready or app_timeout):
            return

        if not shell_ready:
            log_msg("[MONITOR] uart:~ timeout, sending shell cmds anyway")
        else:
            log_msg("[MONITOR] uart:~ ready, sending shell cmds")
        self.shell_cmds_sent = True
        self._shell_cmds_sent_time = time.time()
        time.sleep(1.0)
        for cmd in ZEPHYR_SHELL_CMDS:
            log_msg(f"[MONITOR] Shell: {cmd}")
            self.send_shell_cmd(cmd)
            time.sleep(8.0)
        
    def run(self):
        """运行监控"""
        try:
            self.open_serial()
            
            # 打开日志文件（实时写入）
            if self.log_file:
                self.log_fp = open(self.log_file, 'wb')
            
            # 不要清空串口缓冲区！保留已有数据
            # 这样可以捕捉到 BL1/BL2 的早期输出
            
            # 输出就绪信号（stdout 和文件）
            print("MONITOR_READY")
            sys.stdout.flush()
            Path(self.ready_file).write_text("MONITOR_READY")
            
            start_time = time.time()
            uboot_entered_time = None
            boot_detected = False  # 是否检测到启动
            boot_detect_time = None  # 启动检测时间
            
            while True:
                # 等待 U-Boot 超时（从检测到启动后才开始计时）
                # 这样可以在设备未启动时持续监听
                if boot_detected:
                    boot_elapsed = time.time() - boot_detect_time
                    if boot_elapsed > self.boot_wait and not self.uboot_ready:
                        log_msg("[MONITOR] Timeout waiting for U-Boot prompt")
                        break
                    
                # 启动后捕获超时（Hello World 后额外留时间给 Shell/SMP）
                if self.boot_started:
                    deadline = uboot_entered_time + self.post_boot
                    if self._app_seen_time is not None:
                        deadline = max(deadline, self._app_seen_time + 50)
                    if self._shell_cmds_sent_time is not None:
                        deadline = max(deadline, self._shell_cmds_sent_time + 45)
                    if time.time() > deadline:
                        break

                data_str = ""
                if self.buf_len:
                    data_str = bytes(self.buf[:self.buf_len]).decode(errors='ignore')
                self._maybe_send_shell_cmds(data_str)
                        
                # select 等待数据（高频模式下使用更短的 timeout）
                try:
                    # 高频发送 Enter 时，使用更短的 timeout（10ms）
                    timeout_val = 0.01 if (boot_detected and boot_detect_time and 
                                          time.time() - boot_detect_time < 10 and 
                                          not self.uboot_ready) else 0.08
                    r, w, e = select.select([self.fd], [], [], timeout_val)
                except select.error:
                    continue
                    
                if r:
                    try:
                        chunk = os.read(self.fd, 4096)
                    except OSError:
                        continue
                        
                    if chunk:
                        # 添加到 ring buffer
                        space = len(self.buf) - self.buf_len
                        if len(chunk) > space:
                            # 丢弃旧数据
                            discard = len(chunk) - space
                            self.buf[:discard] = self.buf[discard:self.buf_len]
                            self.buf_len -= discard
                        self.buf[self.buf_len:self.buf_len + len(chunk)] = chunk
                        self.buf_len += len(chunk)
                        
                        # 保存到日志（实时写入）
                        self.log_data.extend(chunk)
                        if self.log_fp:
                            self.log_fp.write(chunk)
                            self.log_fp.flush()  # 立即写入磁盘
                        
                        # 检测启动开始标记（KEYBOX PART - 这是关键停止点）
                        data = bytes(self.buf[:self.buf_len])
                        data_str = data.decode(errors='ignore')
                        
                        # 只有 KEYBOX PART 或 FAT12 触发高频 Enter
                        if not boot_detected:
                            if STOP_MARK in data_str or "FAT12" in data_str:
                                boot_detected = True
                                boot_detect_time = time.time()
                                marker = STOP_MARK if STOP_MARK in data_str else "FAT12"
                                log_msg(f"[MONITOR] Boot marker '{marker}' detected, sending Enter aggressively")
                                # 检测到启动，立即发送第一个 Enter
                                self.send_enter()
                        
                        # 检查标记
                        if self.check_markers():
                            if self.status == "android":
                                break

                        # U-Boot 就绪，执行启动命令
                        if self.uboot_ready and not self.boot_started and self.do_boot:
                            self.boot_started = True
                            uboot_entered_time = time.time()
                            
                            # 等待 prompt 稳定
                            time.sleep(0.5)
                            
                            # 发送启动命令（增加延时确保U-Boot处理每个命令）
                            for cmd in UBOOT_BOOT_CMDS:
                                log_msg(f"[MONITOR] Sending: {cmd}")
                                self.send_line(cmd)
                                time.sleep(1.0)  # 增加到1秒，确保U-Boot完成命令执行
                                
                # autoboot 窗口持续发送 Enter（启动检测后 10 秒，高频发送）
                # 不阻塞主循环，只在每次迭代时发送 Enter
                if boot_detected and boot_detect_time:
                    enter_elapsed = time.time() - boot_detect_time
                    if enter_elapsed < 10 and not self.uboot_ready:
                        # 发送 Enter（不 sleep，避免阻塞）
                        self.send_enter()
            
        finally:
            self.close_serial()

            # 关闭日志文件
            if self.log_fp:
                self.log_fp.close()

            byte_count = len(self.log_data)
            if byte_count > 0:
                print(f"Log saved to {self.log_file} ({byte_count} bytes)")

            # 备用：如果有数据但文件未打开，写入文件
            if self.log_data and not self.log_fp:
                Path(self.log_file).write_bytes(self.log_data)
                print(f"Log saved to {self.log_file} ({byte_count} bytes)")

            print(f"CAPTURE_DONE status={self.status} bytes={byte_count}")
            sys.stdout.flush()
                
def main():
    parser = argparse.ArgumentParser(description="Amlogic S4 串口监控")
    parser.add_argument("--dev", default=os.getenv("DEV", "/dev/ttyUSB0"),
                        help="串口设备")
    
    # 信号处理：确保正常退出时写入日志
    def signal_handler(signum, frame):
        log_msg(f"[MONITOR] Received signal {signum}, exiting...")
        sys.exit(0)
    
    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)
    parser.add_argument("--baud", type=int, default=int(os.getenv("BAUD", "921600")),
                        help="波特率")
    parser.add_argument("--log", default=os.getenv("LOG", "/tmp/s4_boot.log"),
                        help="日志文件")
    parser.add_argument("--boot-wait", type=int, 
                        default=int(os.getenv("BOOT_WAIT_SEC", "80")),
                        help="等待 U-Boot 秒数")
    parser.add_argument("--post-boot", type=int,
                        default=int(os.getenv("POST_BOOT_SEC", "15")),
                        help="启动后捕获秒数")
    parser.add_argument("--no-boot", action="store_true",
                        help="仅停在 U-Boot，不启动")
    parser.add_argument("--ready-file", default="/tmp/s4_monitor_ready",
                        help="就绪信号文件路径")
    
    args = parser.parse_args()
    
    # 检查权限
    if os.geteuid() != 0:
        print("ERROR: 需要 sudo 权限访问串口设备")
        sys.exit(1)
        
    monitor = SerialMonitor(
        dev=args.dev,
        baud=args.baud,
        log_file=args.log,
        boot_wait=args.boot_wait,
        post_boot=args.post_boot,
        do_boot=not args.no_boot,
        ready_file=args.ready_file
    )
    
    monitor.run()
    
if __name__ == "__main__":
    main()