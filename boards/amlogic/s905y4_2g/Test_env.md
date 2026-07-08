# Amlogic S4 TX3 mini plus 测试环境

Zephyr 板级 `s905y4_2g`（S905W2 / S905Y4，4×Cortex-A55）实板调试环境说明。

详细文档见 [doc/](doc/README.md)，镜像打包见 [BOOT.md](BOOT.md)。

## 拓扑

```text
  [编译机] /work/zephyr-rtos/zephyrproject
        │  west build → build_s4_shell/zephyr/zephyr.uimg
        │  scp 到 142 TFTP（或 SMB 同步到 85）
        │
  [142] 192.168.53.142  TFTP server + USB 上下电
        │  /home/lynxi/usb_power/amlogic_s4_reset.sh
        │  TFTP 根: /data/work/tftpboot/
        │
  [85]  192.168.53.85   串口 /dev/ttyUSB0 @ 921600
        │  SMB: /mnt/49.20/zephyr-rtos/zephyrproject
        └── TX3 mini plus (S905W2, 4×A55)
```

## 凭据与地址

| 角色 | 地址/设备 | 说明 |
|------|-----------|------|
| 串口服务器 | `lynxi@192.168.53.85` | 密码 `Lynxi#123+`，需 sudo |
| 串口 | `/dev/ttyUSB0` @ **921600** 8N1 | `sudo screen /dev/ttyUSB0 921600` |
| 供电/TFTP | `lynxi@192.168.53.142` | 密码 `Lynxi#123+`，需 sudo |
| 设备 IP | `192.168.53.130` | U-Boot `ipaddr` |
| TFTP 服务器 | `192.168.53.142` | U-Boot `serverip` |
| 复位脚本 | `/home/lynxi/usb_power/amlogic_s4_reset.sh` | USB 上下电 |

**双 SSH 会话**：85 监控串口，142 部署镜像并复位。

## 前置条件

```bash
sudo apt install sshpass   # 编译机
```

- 跑自动化前**退出 `screen`**，避免占用 `/dev/ttyUSB0`
- 串口波特率必须为 **921600**
- UART_B 基址 `0xFE07A000`（Meson UART，非 ns16550）
- SECMON 区域 `0x05000000~0x08200000` 须避开（见 [doc/porting_issues.md](doc/porting_issues.md)）

## 编译

```bash
cd /work/zephyr-rtos/zephyrproject
source zephyr/zephyr-env.sh
west build -b s905y4_2g -d build_s4_shell -s zephyr/samples/hello_world --pristine
```

产物：`build_s4_shell/zephyr/zephyr.uimg`（load=entry=**0x01000000**）。

## 部署

TFTP 目录在 142 上为 `/data/work/tftpboot/`：

```bash
sshpass -p 'Lynxi#123+' scp build_s4_shell/zephyr/zephyr.uimg \
    lynxi@192.168.53.142:/data/work/tftpboot/
```

`board_test.sh deploy` 会自动执行上述 scp 并确认文件大小。

## 自动化测试

脚本路径：`zephyr/boards/amlogic/s905y4_2g/board_test.sh`

```bash
cd /work/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g
./board_test.sh help          # 查看命令
./board_test.sh build         # 仅编译
./board_test.sh deploy        # 仅部署 TFTP
./board_test.sh reset         # 仅 USB 复位
./board_test.sh uboot         # 复位 + 监控，停在 U-Boot（不发 TFTP boot）
./board_test.sh boot          # 完整流程（推荐）
./board_test.sh analyze       # 分析最近一次日志
./board_test.sh monitor       # 仅监控串口
```

### 命令说明

| 命令 | 作用 |
|------|------|
| `build` | `west build -b s905y4_2g --pristine` |
| `deploy` | scp `zephyr.uimg` → 142 TFTP |
| `reset` | 142 执行 `amlogic_s4_reset.sh` |
| `uboot` | 双线程：85 监控 + 142 复位，**不**自动 TFTP 启动 |
| `boot` | 编译 → 部署 → 双线程启动 → 自动分析日志 |
| `monitor` | 后台启动 `remote_serial_test.py` |
| `analyze` | 解析 `/tmp/s4_boot_*.log` |

### `boot` 双线程流程

```text
线程 a (85): remote_serial_test.py 后台监控 /dev/ttyUSB0
线程 b (142): 监控就绪 3s 后 USB 复位
             → U-Boot 自动 TFTP 加载 zephyr.uimg
             → 捕获串口至 /tmp/s4_boot_YYYYMMDD_HHMMSS.log
             → 输出 CAPTURE_DONE
编译机:      scp 日志到 /tmp/s4_boot_*.log → analyze_log
```

日志位置：

| 文件 | 位置 | 内容 |
|------|------|------|
| 串口捕获 | 85: `/tmp/s4_boot_*.log` | 完整 boot 日志 |
| 监控 stdout | 85: `/tmp/s4_monitor_stdout.log` | Python 脚本输出 |
| 本地副本 | 编译机: `/tmp/s4_boot_*.log` | `analyze` 使用 |

### 验收标准

**通过**（`./board_test.sh boot` 输出 `[OK]`）：

1. 检测到 U-Boot prompt `ap201#`
2. TFTP `Bytes transferred = ...`
3. 出现 `Zephyr SPL (S4)`
4. Boot markers 至少包含 `HGgLP12345` 及后续 MMU/内核 marker
5. 最终出现 **`Hello World! s905y4_2g/meson_s4`** 或 `uart:~$`

**失败**典型现象：

| 现象 | 可能原因 |
|------|----------|
| 无串口 | 85 权限/波特率/screen 占用 |
| TFTP 失败 | 142 网络、`zephyr.uimg` 不存在 |
| `Starting kernel` | 进了 Android，U-Boot 未跑 `uenvcmd` |
| `HGgLP` + 乱码 | UART TX_EMPTY 未等待（见 [doc/porting_issues.md](doc/porting_issues.md)） |
| `HGgLP12345` 后停 | MMU/dcache 问题 |
| markers 到 `k` 后停 | PRE_KERNEL_1 UART init（见 [doc/porting_issues.md](doc/porting_issues.md)） |

成功启动示例（boot trace 开启时）：

```text
Zephyr SPL (S4)
HGgLP12345abCdefg1212jk...
Hello World! s905y4_2g/meson_s4
*** Booting Zephyr OS build ...
```

Boot marker 速查见 [doc/debug.md](doc/debug.md) 和 [doc/zephyr_boot_flow.md](doc/zephyr_boot_flow.md)。

### 手动测试（无脚本）

**85 — 监控串口：**

```bash
ssh lynxi@192.168.53.85
echo 'Lynxi#123+' | sudo -S screen /dev/ttyUSB0 921600
```

**142 — 复位 + 确认 TFTP：**

```bash
ssh lynxi@192.168.53.142
echo 'Lynxi#123+' | sudo -S /home/lynxi/usb_power/amlogic_s4_reset.sh
ls -lh /data/work/tftpboot/zephyr.uimg
```

**U-Boot 手动 TFTP 启动**（autoboot 窗口内按键打断）：

```text
setenv serverip 192.168.53.142
setenv ipaddr 192.168.53.130
setenv loadkernel tftpboot 0x01000000 zephyr.uimg
setenv uenvcmd "run loadkernel; bootm 0x01000000"
run uenvcmd
```

TFTP 成功时输出 `Bytes transferred = ...`。

## U-Boot 交互参数

| 参数 | 值 |
|------|-----|
| U-Boot prompt | `s4_ap201#`（脚本匹配 `ap201#`） |
| STOP_MARK | `KEYBOX PART` / `FAT12` |
| AUTOBOOT_MARK | `Hit any key to stop autoboot` |
| FAIL_MARK | `Starting kernel`（进入 Android） |
| bootdelay | 1 |

## Shell 验收（启动成功后）

```text
uart:~$ meson_s4_smp
uart:~$ kernel threads
uart:~$ device list
uart:~$ reboot
```

> 注：当前 hello_world 最小配置可能未启用 Shell；以 `Hello World!` 输出为主要验收项。

## 相关文档

| 文件 | 内容 |
|------|------|
| [doc/README.md](doc/README.md) | 文档索引 |
| [doc/debug.md](doc/debug.md) | 调试方法、boot marker、故障树 |
| [doc/bl33_to_zephyr.md](doc/bl33_to_zephyr.md) | BL33 跳转分析 |
| [doc/zephyr_boot_flow.md](doc/zephyr_boot_flow.md) | Zephyr 启动详细流程 |
| [doc/porting_issues.md](doc/porting_issues.md) | 移植问题与解决方案 |
| [BOOT.md](BOOT.md) | uimg 打包、load 地址 |
| `board_test.sh` | 自动化脚本 |
