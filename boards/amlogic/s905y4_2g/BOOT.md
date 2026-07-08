# Amlogic S4 (S905Y4/S905W2) Zephyr 启动文档

## 1. 加载地址与运行地址

Amlogic S4 的 ARM Trusted Firmware (BL31) 定义 NS_BL33_ENTRYPOINT = `0x01000000`，
即 BL33 (U-Boot 或 Zephyr) 的入口地址。本 Zephyr 镜像遵循 baremetal_test 的打包方式：

**加载地址 = 运行地址 = `0x01000000`**

| 项目            | 地址          | 说明                                |
|-----------------|---------------|-------------------------------------|
| BL33 入口地址   | 0x01000000    | BL31 加载 BL33 后跳转的地址          |
| mkimage -a      | 0x01000000    | uImage 加载地址（Load Address）     |
| mkimage -e      | 0x01000000    | uImage 入口地址（Entry Point）      |
| ELF Entry Point | 0x01000000+偏移 | Zephyr _start 在 0x01000000 区域内 |
| 代码段起始      | 0x01000000    | __text_region_start = 0x01000000    |

### 与 baremetal_test 的对比

```
baremetal_test:
  baremetal_linker.lds: . = 0x01000000;  (代码从 0x01000000 开始)
  mkimage: -a 0x01000000 -e 0x01000000  (加载地址 = 入口地址)

Zephyr s905y4_2g:
  linker.ld: ROM_ADDR/RAM_ADDR = 0x01000000 (代码从 0x01000000 开始)
  mkimage: -a 0x01000000 -e 0x01000000  (加载地址 = 入口地址)
```

两者完全一致：镜像被 U-Boot 的 `bootm` 命令加载到 0x01000000 后直接在该地址运行，
无需二次搬运（因为 -a 和 -e 相同，bootm 判断当前地址 = 加载地址时跳过复制）。

### 为什么选择 0x01000000？

1. Amlogic S4 BL31 (ARM Trusted Firmware) 将 NS_BL33_ENTRYPOINT 设为 0x01000000
2. U-Boot 的 `bootm` 命令将 uImage 解压/加载到 -a 地址，然后跳转到 -e 地址执行
3. 当 -a = -e 时，镜像只需加载一次，直接在原位运行，效率最高
4. BL31 通过 PSCI CPU_ON 将其他 CPU 核心引导到同一地址范围的代码中

## 2. 镜像打包流程

### 2.1 编译 Zephyr

```bash
cd /work/zephyr-rtos/zephyrproject
west build -b s905y4_2g -d build_s4 -s zephyr/samples/hello_world --pristine
```

### 2.2 生成 zephyr.bin

编译完成后自动生成：
```
build_s4/zephyr/zephyr.bin   — 纯二进制文件 (~100KB)
build_s4/zephyr/zephyr.elf   — ELF 文件 (含符号表)
```

### 2.3 mkimage 打包 uImage

```bash
mkimage -A arm64 -O u-boot -T standalone -C none \
    -a 0x01000000 -e 0x01000000 \
    -n "Zephyr S4 S905Y4" \
    -d build_s4/zephyr/zephyr.bin \
    build_s4/zephyr/zephyr.uimg
```

参数说明（与 baremetal_test 完全对应）：

| 参数 | 值 | 含义 |
|------|-----|------|
| -A arm64 | ARM64 架构 | AArch64 |
| -O u-boot | U-Boot OS 类型 | standalone 程序的 OS 标识 |
| -T standalone | 独立程序类型 | bootm 直接跳转到入口 |
| -C none | 无压缩 | Zephyr bin 无需解压 |
| -a 0x01000000 | 加载地址 | BL33 运行地址 |
| -e 0x01000000 | 入口地址 | 与加载地址相同 |
| -d zephyr.bin | 输入文件 | Zephyr 纯二进制 |

### 2.4 验证 uImage

```bash
mkimage -l build_s4/zephyr/zephyr.uimg
```

预期输出：
```
Image Name:   Zephyr S4 S905Y4
Image Type:   AArch64 U-Boot Standalone Program (uncompressed)
Data Size:    102824 Bytes = 100.41 KiB = 0.10 MiB
Load Address: 01000000
Entry Point:  01000000
```

## 3. 启动方式

### 3.1 从 SD 卡启动

将 `zephyr.uimg` 复制到 SD 卡 FAT 分区，U-Boot 串口终端：

```bash
# 查看可用 MMC 设备
mmc list

# 从 SD 卡加载 uImage 到内存（SD 卡通常是 mmc 1）
fatload mmc 1 0x01000000 zephyr.uimg

# 启动
bootm 0x01000000
```

### 3.2 从 eMMC 启动

```bash
# eMMC 通常是 mmc 0
fatload mmc 0 0x01000000 zephyr.uimg
bootm 0x01000000
```

### 3.3 从 TFTP 启动

```bash
# 设置网络
setenv ipaddr 192.168.1.2
setenv serverip 192.168.1.1

# 从 TFTP 服务器加载
tftp 0x01000000 zephyr.uimg
bootm 0x01000000
```

### 3.4 bootm standalone 处理逻辑

U-Boot `bootm` 对 IH_TYPE_STANDALONE 类型镜像的处理：

1. 解析 uImage 头部，获取加载地址 (-a=0x01000000) 和入口地址 (-e=0x01000000)
2. 检查当前地址是否等于加载地址
3. **若相等**：跳过镜像复制（已在正确位置），直接跳转入口
4. **若不等**：先将镜像从当前地址复制到加载地址，然后跳转入口
5. 以 `appl(argc, argv)` 函数指针形式调用入口，裸机程序可忽略参数直接执行

由于我们的 -a = -e = 0x01000000，步骤 2 判断相等，跳过复制，效率最优。

## 4. 多核启动流程

S4 是 4 核 Cortex-A55 SoC，使用 PSCI (arm,psci-1.0 smc) 启动辅助 CPU：

```
启动流程:
  BL1 (ROM) → BL2 (Bootloader) → BL31 (ARM TF, EL3)
      ↓
  BL31 加载 BL33 (Zephyr) 到 0x01000000
      ↓
  Zephyr 主核 (CPU0) 从 0x01000000 开始执行
      ↓
  Zephyr 通过 PSCI CPU_ON SMC 调用唤醒 CPU1/CPU2/CPU3
      ↓
  4 核全部在线，SMP 调度器正常工作
```

Zephyr 配置：
- `CONFIG_SMP=y` — SMP 支持
- `CONFIG_MP_MAX_NUM_CPUS=4` — 最大 4 核
- `CONFIG_PM_CPU_OPS=y` — CPU 电源管理驱动
- `CONFIG_PM_CPU_OPS_PSCI=y` — PSCI 1.0 SMC 接口

## 5. 内存布局

```
物理地址空间 (S4):
  0x00000000 ~ 0x00FFFFFF:  保留 (BL31/BL32/BL33 安全区域)
  0x01000000 ~ 0x7FFFFFFF: DDR 内存 (Zephyr 运行区域)
      0x01000000:  Zephyr 入口 (__text_region_start)
      0x01000000 + 代码大小: 数据段
      0x01000000 + 代码+数据: BSS 段
  0xFDC00000:             ETH (DW MAC 3.70a)
  0xFDE00000:             XHCI USB
  0xFE000000 ~ 0xFE47FFFF: APB4 外设总线
      0xFE002100:   Watchdog
      0xFE003C000: USB2 PHY0
      0xFE003E000: USB2 PHY1
      0xFE07A000:   UART_B (串口控制台)
      0xFE08A000:   SD 卡 MMC
      0xFE08C000:   eMMC
  0xFFF01000 ~ 0xFFF0FFFF: GIC-400 (中断控制器)
```

## 6. 串口控制台

- **UART_B**: 基址 0xFE07A000 (Amlogic Meson UART，非 ns16550)
- **中断**: GIC SPI #169
- **波特率**: 921600
- **时钟**: 24MHz XTAL
- **引脚**: pinctrl ao_uart_pins (GPIOB_0/GPIOB_1)

串口终端设置：
```
波特率: 921600
数据位: 8
停止位: 1
校验:   None
流控:   None
```

## 7. 构建与部署

### 7.1 构建 Shell Console（推荐）

Board defconfig 已内置 Shell、SMP、logging，构建 hello_world sample 即可获得
完整的 Shell Console（交互式 SMP 状态、设备列表、内核信息等）：

```bash
cd /path/to/zephyrproject
west build -b s905y4_2g -d build_s4_shell -s zephyr/samples/hello_world --pristine
```

### 7.2 打包 uImage

编译完成后自动生成 `zephyr.bin` 和 `zephyr.uimg`（board CMakeLists.txt 的
`extra_post_build_commands` 步骤自动调用 mkimage）。也可手动打包：

```bash
mkimage -A arm64 -O u-boot -T standalone -C none \
    -a 0x01000000 -e 0x01000000 \
    -n "Zephyr S4 S905Y4" \
    -d build_s4_shell/zephyr/zephyr.bin \
    build_s4_shell/zephyr/zephyr.uimg
```

### 7.3 部署到 SD 卡

```bash
# 方法 1：使用 deploy_sd.sh 脚本（自动打包 + 复制）
bash boards/amlogic/s905y4_2g/deploy_sd.sh build_s4_shell /dev/sdb1

# 方法 2：手动复制
mkdir -p /mnt/sdcard
mount /dev/sdb1 /mnt/sdcard
cp build_s4_shell/zephyr/zephyr.uimg /mnt/sdcard/
sync
umount /mnt/sdcard
```

### 7.4 U-Boot 启动命令

```bash
# 查看可用 MMC 设备
mmc list

# 从 SD 卡加载（SD 卡通常是 mmc 1）
fatload mmc 1 0x01000000 zephyr.uimg
bootm 0x01000000

# 从 eMMC 加载（eMMC 通常是 mmc 0）
fatload mmc 0 0x01000000 zephyr.uimg
bootm 0x01000000

# 从 TFTP 加载
setenv ipaddr 192.168.1.2
setenv serverip 192.168.1.1
tftp 0x01000000 zephyr.uimg
bootm 0x01000000
```

## 8. 与 baremetal_test 的完整对比

| 项目 | baremetal_test | Zephyr s905y4_2g |
|------|---------------|------------------|
| 源码 | baremetal_uart.S | 多文件 C + 汇编 |
| 链接地址 | 0x01000000 | 0x01000000 |
| mkimage -a | 0x01000000 | 0x01000000 |
| mkimage -e | 0x01000000 | 0x01000000 |
| mkimage -T | standalone | standalone |
| mkimage -A | arm64 | arm64 |
| mkimage -C | none | none |
| 镜像大小 | 303 B (0.3KB) | 102824 B (~100KB) |
| bootm 启动 | fatload mmc 1 + bootm | fatload mmc 1 + bootm |
| 串口 | UART_B @ 0xFE07A000 | UART_B @ 0xFE07A000 |
| 多核 | 不支持 | PSCI SMP 4核 |

两者镜像格式完全相同，U-Boot 启动命令完全相同，仅镜像内容不同（baremetal 是汇编裸机，Zephyr 是完整 RTOS）。

## 9. 功能验证

### 9.1 多核 (SMP) 验证

启动后通过 Shell 检查 CPU 状态：

```
uart:~$ meson_s4_smp
SMP: PSCI arm,psci-1.0 smc
Online CPUs: 4 / 4 (max: 4)

uart:~$ kernel threads
```

预期：4 个 Cortex-A55 核心全部在线。

### 9.2 eMMC / SD 卡验证

```
uart:~$ device list
```

预期输出中应包含 MMC 设备：
- `SD` — SD 卡（mmc@fe08a000, bus-width=4）
- `SD2` — eMMC（mmc@fe08c000, bus-width=8）

如果启用文件系统支持（`CONFIG_FILE_SYSTEM` + `CONFIG_FS_FATFS`），可以：
```
uart:~$ ls /SD:/
uart:~$ ls /SD2:/
```

### 9.3 Ethernet 验证

DTS 已启用 ETH（RMII, internal PHY @ `phy@8`）。如果启用网络栈（`CONFIG_NET_L2_ETHERNET`）：

```
uart:~$ net iface
uart:~$ net ping 192.168.1.1
```

DW MAC 3.70a 的平台 glue（`eth_dwmac_meson_s4.c`）负责时钟/PHY 选择；
完整 DWMAC 驱动功能需要 Zephyr 主线 `eth_dwmac.c` 支持（当前状态：开发中）。

### 9.4 USB 验证

DTS 已定义 USB host 节点（`generic-xhci @ 0xFDE00000`）。
Zephyr 的 XHCI 驱动为 `CONFIG_USB_HOST_DRIVER_XHCI`。
当前状态：DTS 就绪，驱动层待完善。

### 9.5 串口控制台验证

启动后串口应自动出现 Zephyr Shell 提示符 `uart:~$`。
检查 UART 配置：

```
uart:~$ device list
```

应包含 `uart_b` 设备（`amlogic,meson-s4-uart` @ 0xFE07A000）。

## 10. 驱动功能状态汇总

| 模块 | 驱动 | DTS | 配置 | 状态 |
|------|------|-----|------|------|
| UART_B | `uart_meson.c` | ✅ | `CONFIG_UART_MESON` | ✅ 已验证 |
| SD 卡 | `sdhc_meson_axg_mmc.c` | ✅ | `CONFIG_SDHC_MESON_AXG_MMC` | ⚠️ 开发中 |
| eMMC | `sdhc_meson_axg_mmc.c` | ✅ | `CONFIG_SDHC_MESON_AXG_MMC` | ⚠️ 开发中 |
| Ethernet | `eth_dwmac_meson_s4.c` | ✅ | `CONFIG_ETH_DWMAC_MESON_S4` | ⚠️ 开发中 |
| USB XHCI | 无 Zephyr 驱动 | ✅ | DTS only | ❌ 待实现 |
| SMP / PSCI | Zephyr PSCI | ✅ | `CONFIG_PM_CPU_OPS_PSCI` | ✅ 已验证 |
| I2C | 无 Zephyr 驱动 | ✅ | DTS only | ❌ 待实现 |
| Watchdog | 无 Zephyr 驱动 | ✅ | DTS only | ❌ 待实现 |

## 11. 启动卡住问题调试记录（2026-07-02）

### 11.1 现象

Zephyr 通过 U-Boot `bootm 0x01000000` 加载后，串口输出：
```
Zephyr SPL (S4)
HGgLP
```
然后在 `P`（`soc_prep_hook()`）之后完全卡住，无任何进一步输出。

### 11.2 Boot Marker 分析

启用 `CONFIG_SOC_MESON_S4_BOOT_TRACE=y` 后，各 marker 对应���置：

| Marker | 来源文件 | 函数 | 阶段 |
|--------|---------|------|------|
| `S` | `meson_s4_boot_debug.S` | reset hook | EL 最高层 |
| `H` | `meson_s4_plat.c` | `z_arm64_el_highest_plat_init()` | 定时器设置 |
| `G`/`g` | `meson_s4_plat.c` | `z_arm64_el2_plat_init()` | EL2 平台初始化 |
| `L` | `meson_s4_plat.c` | `z_arm64_el1_plat_init()` | 清除 SCTLR.C/M |
| `P` | `meson_s4_plat.c` | `soc_prep_hook()` | 准备 C 运行环境 |
| `1`~`4` | `prep_c.c` | `z_prep_c()` | .bss/.data 初始化 |
| `A`~`D`/`a`~`g` | `mmu.c`/`prep_c.c` | MMU init | 页表建立 |
| `5`/`6`/`i`/`z`/`Z` | `prep_c.c` | interrupt/z_cstart | 中断/内核初始化 |
| `M`/`I`/`C`/`K` | `init.c` | SYS_INIT levels | 驱动初始化 |

### 11.3 调试过程

#### 尝试 1: dcache clean 修复
**假设**：BL31/U-Boot 有 dirty dcache lines，MMU 开启后读到错误页表。
**修改**：在 `enable_mmu_el1()` 中添加 dcache clean/invalidate。
**结果**：无效。日志仍停在 `4` 或 `a`。

#### 尝试 2: 投票锁 .bss 清零
**假设**：`arm64_cpu_boot_params.voting[]` 在 .bss 清零前有垃圾数据，导致主核无限等待其他核投票。
**修改**：在 `reset.S` 投票锁之前手动清零 `voting[]` 数组。
**结果**：部分有效。SMP 时从 `LP` 变为 `HGgLP`，但单核���仍然停在 `P`。

#### 尝试 3: 禁用 SMP 隔离问题
**修改**：`CONFIG_SMP=n`。
**结果**：marker 序列变为 `pP1234a`，说明 MMU 初始化成功（`a` 在 `z_arm64_mm_init()` 之后），但 `z_arm64_interrupt_init()` 返回后卡住。

### 11.4 根因分析

**最终 marker 序列**（SMP 禁用 + 详细 marker）：
```
pP1234a
```
- `p` = U-Boot autoboot Enter 误识别
- `P` = `soc_prep_hook()` ✓
- `1` = after `soc_prep_hook()` ✓
- `2` = after `write_tpidrro_el0()` ✓
- `3` = after `arch_bss_zero()` ✓
- `4` = after `arch_data_copy()` ✓
- `a` = after `z_arm64_mm_init(true)` ✓
- **卡住** = `z_arm64_interrupt_init()` 返回后

**关键发现**：
1. `z_arm64_mm_init()` 成功返回（`a` marker 输出），MMU 已开启，dcache 已开启
2. `z_arm64_interrupt_init()` 是空函数（无 `CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER`）
3. 卡住发生在 `z_arm64_interrupt_init()` 返回后、`meson_s4_boot_marker('5')` 之前
4. `meson_s4_boot_marker('5')` 需要访问 UART 寄存器 `0xFE07A000`

**最可能的根因**：
- MMU 开启后，`meson_s4_boot_marker` 函数通过 MMU 访问 UART 寄存器
- UART 寄存器 `0xFE07A000` 在 APB4 映射中（`0xFE000000, 0x480000, MT_DEVICE_nGnRnE`）
- 但 dcache 中有来自 BL31 的 dirty data，在 MMU+dcache 同时开启后，页表遍历可能读到 stale cache lines
- 导致 Data Abort 或访问未映射地址

**为什么 `G` 和 `P` 能输出但 `5` 不能**：
- `G` 在 EL2 上输出（dcache 可能未开启或干净）
- `P` 在 `soc_prep_hook()` 中输出（此时 dcache 已被 `z_arm64_el1_plat_init()` 关闭）
- `5` 在 MMU+dcache 开启后输出（此时 dcache 可能有 dirty data）

### 11.5 待验证的修复方案

1. **在 `enable_mmu_el1()` 中分步开启 MMU 和 dcache**：
   - 先开启 MMU（关 dcache）→ 确保页表遍历使用 RAM 数据
   - 再开启 dcache → 此时页表已在 RAM 中且干净

2. **在 `z_arm64_el1_plat_init()` 中清除 SCTLR.C 之前做 dcache clean**：
   - 使用 set/way clean+invalidate（不依赖 dcache 状态）
   - 确保 BL31 的 dirty data 被写回 RAM

3. **临时方案：禁用 dcache 直到所有驱动初始化完成**：
   - 在 `enable_mmu_el1()` 中只开启 MMU，不开 dcache
   - 在 `z_cstart()` 之后手动开启 dcache
   - 类似 Linux 内核的做法

### 11.8 最终修复（2026-07-08）

根因与完整记录见 [doc/porting_issues.md](doc/porting_issues.md)。简要结论：

1. **UART TX_EMPTY 等待**（`meson_s4_early_uart.c`）— 主因
2. **分层 cache 维护** — EL2 disable、EL1 flush、MMU 使能前 flush、z_cstart flush
3. **PRE_KERNEL_1 UART skip init** — 见 [doc/porting_issues.md § UART](doc/porting_issues.md)

BL33 跳转分析：[doc/bl33_to_zephyr.md](doc/bl33_to_zephyr.md)  
启动流程：[doc/zephyr_boot_flow.md](doc/zephyr_boot_flow.md)  
自动化测试：`./board_test.sh boot`（见 [Test_env.md](Test_env.md)）。

| 文件 | 作用 |
|------|------|
| `arch/arm64/core/reset.S` | 汇编入口、投票锁、EL 切换 |
| `arch/arm64/core/prep_c.c` | C 运行环境准备 |
| `arch/arm64/core/mmu.c` | MMU 页表建立和使能 |
| `soc/amlogic/s4/meson_s4_plat.c` | 平台初始化（EL2/EL1） |
| `soc/amlogic/s4/meson_s4_early_uart.c` | 早期 UART 输出（boot marker） |
| `soc/amlogic/s4/mmu_regions.c` | 外设 MMU 映射 |
| `arch/arm64/core/irq_init.c` | 中断初始化 |
| `kernel/init.c` | 内核初始化（z_cstart） |