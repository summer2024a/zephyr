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
- **波特率**: 115200
- **时钟**: 24MHz XTAL
- **引脚**: pinctrl ao_uart_pins (GPIOB_0/GPIOB_1)

串口终端设置：
```
波特率: 115200
数据位: 8
停止位: 1
校验:   None
流控:   None
```

## 7. 快速启动脚本

### deploy_sd.sh — 部署到 SD 卡

```bash
#!/bin/bash
set -e

BIN_DIR="${1:-build_s4/zephyr}"
SD_DEV="${2:-/dev/sdb1}"   # SD 卡 FAT 分区设备
MNT="/mnt/sdcard"

# 编译
cd /work/zephyr-rtos/zephyrproject
west build -b s905y4_2g -d build_s4 -s zephyr/samples/hello_world

# 打包 uImage
mkimage -A arm64 -O u-boot -T standalone -C none \
    -a 0x01000000 -e 0x01000000 \
    -n "Zephyr S4 S905Y4" \
    -d build_s4/zephyr/zephyr.bin \
    build_s4/zephyr/zephyr.uimg

# 复制到 SD 卡
mkdir -p $MNT
mount $SD_DEV $MNT
cp build_s4/zephyr/zephyr.uimg $MNT/
sync
umount $MNT

echo "部署完成！将 SD 卡插入设备后，在 U-Boot 中执行:"
echo "  fatload mmc 1 0x01000000 zephyr.uimg"
echo "  bootm 0x01000000"
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