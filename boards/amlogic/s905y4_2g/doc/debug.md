# S4 实板调试方法

Board: `s905y4_2g` / SoC: `meson_s4`

环境拓扑与编译部署见 [Test_env.md](../Test_env.md)。

## 1. 自动化调试（推荐）

```bash
cd /work/zephyr-rtos/zephyrproject/zephyr/boards/amlogic/s905y4_2g
./board_test.sh boot          # 编译 + 部署 + TFTP 启动 + 分析日志
./board_test.sh analyze       # 仅分析最近一次日志
./board_test.sh uboot         # 停在 U-Boot，不发 TFTP boot
```

### 双线程流程

```text
线程 a (85): remote_serial_test.py 监控 /dev/ttyUSB0 @ 921600
线程 b (142): 监控就绪 3s 后 USB 复位 → U-Boot TFTP 加载 zephyr.uimg
编译机:      scp 日志 → analyze_log
```

### 日志位置

| 文件 | 位置 |
|------|------|
| 串口捕获 | 85: `/tmp/s4_boot_YYYYMMDD_HHMMSS.log` |
| 监控 stdout | 85: `/tmp/s4_monitor_stdout.log` |
| 本地副本 | 编译机: `/tmp/s4_boot_*.log` |

### 验收标准

1. U-Boot prompt `ap201#`
2. TFTP `Bytes transferred = ...`
3. `Zephyr SPL (S4)`
4. Boot markers 至少到 `HGgLP12345` 及后续 MMU/内核 marker
5. **`Hello World! s905y4_2g/meson_s4`** 或 `uart:~$`

## 2. 手动调试

**85 — 串口：**

```bash
ssh lynxi@192.168.53.85
echo 'Lynxi#123+' | sudo -S screen /dev/ttyUSB0 921600
```

**142 — 复位：**

```bash
ssh lynxi@192.168.53.142
echo 'Lynxi#123+' | sudo -S /home/lynxi/usb_power/amlogic_s4_reset.sh
```

**U-Boot TFTP：**

```text
setenv serverip 192.168.53.142
setenv ipaddr 192.168.53.130
setenv loadkernel tftpboot 0x01000000 zephyr.uimg
setenv uenvcmd "run loadkernel; bootm 0x01000000"
run uenvcmd
```

## 3. Boot Marker 调试

启用 `CONFIG_SOC_MESON_S4_BOOT_TRACE=y` 后，早期启动输出单字符 marker，用于定位 hang 位置。

| 字符 | 阶段 |
|------|------|
| `H`/`G`/`g`/`L` | EL 最高 / EL2 / EL1 plat init |
| `P` | `soc_prep_hook()` |
| `1`~`4` | `z_prep_c()` (.bss/.data) |
| `5`/`a`/`b` | MMU init / interrupt / 进入 `z_cstart` |
| `C`~`g`/`h`/`i`/`j`/`k` | `z_cstart()` 各子步骤 |
| `l`/`m`/`s`~`u` | PRE_KERNEL_1/2 驱动 init |
| `! ESR=...` | Data Abort / fault |

成功序列示例：

```text
Zephyr SPL (S4)
HGgLP12345abCdefg1212jk...
Hello World! s905y4_2g/meson_s4
```

完整启动阶段对应关系见 [zephyr_boot_flow.md](zephyr_boot_flow.md)。

## 4. 故障树

```text
无串口输出
  → 85 /dev/ttyUSB0 权限与 921600 波特率
  → screen 是否占用串口
  → 142 reset 脚本是否成功

U-Boot TFTP 失败
  → ping 192.168.53.142
  → /data/work/tftpboot/zephyr.uimg 是否存在
  → ipaddr/serverip 是否正确

卡在 HGgLP 后乱码
  → meson_s4_early_uart.c TX_EMPTY 等待
  → z_cstart() / enable_mmu_el1() cache flush

HGgLP12345 后停（无 a/b）
  → MMU 页表 / dcache 策略（见 porting_issues.md § MMU）

markers 到 k 后停（PRE_KERNEL_1）
  → meson_uart_init 跳过 virt map（见 porting_issues.md § UART）

Starting kernel（Android）
  → U-Boot 未执行 uenvcmd，进了 Android 分区
```

## 5. 常用 Kconfig 调试开关

| 选项 | 作用 |
|------|------|
| `CONFIG_SOC_MESON_S4_BOOT_TRACE` | 早期 UART boot marker |
| `CONFIG_ARM64_BOOT_DISABLE_DCACHE` | EL2 入口 flush + 关 dcache |
| `CONFIG_ARM64_DCACHE_ALL_OPS` | 使用 set/way 全局 cache 操作 |
| `CONFIG_SMP=n` | 单核调试，排除投票锁干扰 |

## 6. 关键源文件

| 文件 | 作用 |
|------|------|
| `board_test.sh` | 自动化编译/部署/复位/分析 |
| `remote_serial_test.py` | 85 串口监控 |
| `soc/amlogic/s4/meson_s4_early_uart.c` | 早期 UART marker |
| `soc/amlogic/s4/meson_s4_plat.c` | EL2→EL1、SCTLR fixup |
| `arch/arm64/core/mmu.c` | MMU 页表与使能 |
| `arch/arm64/core/prep_c.c` | C 运行环境准备 |
| `kernel/init.c` | `z_cstart()` |
| `drivers/serial/uart_meson.c` | 正式 UART 驱动 |

## 7. 注意事项

- 跑自动化前退出 `screen`，避免占用 `/dev/ttyUSB0`
- UART_B @ `0xFE07A000`，921600 8N1
- SECMON `0x05000000~0x08200000` 须避开（BL31 Secure-only）
- `source zephyr-env.sh` 会覆盖 `ZEPHYR_BASE`；脚本使用独立 `ZEPHYR_PROJECT` 变量
