# Lynxi KA200 / HE200 Zephyr 移植说明

本文档记录 Zephyr 在 Lynxi HE200（SoC：`lynxi,ka200`）上的板级移植、**U-Boot SPL 直连启动**（将 `zephyr.bin` 当作 `u-boot.bin` 烧录）过程中的问题与修复，以及建议的 Git 提交划分。

对照参考：RT-Thread BSP `bsp/lynxi/he200`（链接地址 `0x800100000`、CPR 时钟门控、MMU 设备区映射等）。

---

## 1. 范围与板型

| 板名 | Kconfig | 用途 | SPL 直连验证 |
|------|---------|------|----------------|
| `he200` | `BOARD_HE200` | 通用 HE200 开发板 DTS/配置 | 未作为默认 SPL 目标 |
| `he200_ep` | `BOARD_HE200_EP` | Entry Point：与 SPL 入口 `0x800100000` 对齐 | **已验证**（Shell + `app_shell_fs` + **8 核 SMP**） |

说明：

- 仓库中当前仅有 **`he200` / `he200_ep`**，无单独 `he200_rc` 目录；若 RC 指另一产品形态，可复用本 SoC 与 `he200_common.dtsi`，按板级 defconfig/linker 拆分。
- SoC 系列：`SOC_LYNXI_KA200`（`zephyr/soc/lynxi/ka200/`）。

### 1.1 命名分层：KA200 vs KA200_EP（移植必读）

产品线约定（与 RT/Linux 一致）：

| 层级 | 名称 | 含义 | Zephyr 落点 |
|------|------|------|-------------|
| **SoC** | **KA200** | 芯片共性：8 核、GICv3、CPR、APB 外设、SMP spin-table、eMMC/GMAC/I2C 等 | `soc/lynxi/ka200/`、`SOC_LYNXI_KA200`、`boards/lynxi/common/he200_*.dtsi` |
| **硅片/产品 SKU** | **KA200_EP** vs **KA200_RC**（等） | **仅 PCIe 相关不同**（EP 设备、DBI/MSI-X、rpmsg-net、CPR `0x84` 门控等）；其余 IP **与 KA200 相同** | 未来：`CONFIG_*_KA200_EP`、PCIe DTS 节点、板级 `defconfig` 开关；**不要**为 EP 再写一套 eMMC/UART/SMP |
| **Zephyr 板目标** | **`he200` / `he200_ep`** | 镜像/启动形态（linker 入口、SPL 直连、早期 UART 调试），**不是**第二颗 SoC | `boards/lynxi/he200*`；`he200_ep` 当前 = SPL @ `0x800100000` 已验板 |

**配置与代码放置规则：**

1. **可复用驱动**（eMMC `lynxi,dwcmshc-sdhci`、sysctl、MMU 设备区、spin-table、`uart_ns16550` KA200 quirk）→ 放在 **`soc/lynxi/ka200/`** 或 **`drivers/` + 公共 `he200_peripherals.dtsi`**，Kconfig 挂在 `SOC_LYNXI_KA200` / `DT_HAS_*`，**禁止**用 `BOARD_HE200_EP` 包裹整段外设逻辑。
2. **仅 EP 差异** → PCIe 驱动、rpmsg、EP 专用时钟门（`drv_sysctl_lite.c` 中 `0x84` 的 pcie_* bit）、EP DTS `compatible`；命名建议 **`KA200_EP` / `CONFIG_LYNXI_KA200_PCIE_EP`**，与板名 `he200_ep` 解耦（板名可继续表示“该 SKU 的默认烧录方式”）。
3. **板级独有、与 SoC IP 无关** → SPL linker、`he200_ep_spl_mmu_prepare` 调用点、early UART 阶段字符、`he200_ep_defconfig` 默认 SMP/Shell；保留 **`HE200_EP_*` / `BOARD_HE200_EP`** 前缀。
4. **新增板型**（如 `he200_rc`）→ 只新增 **板目录 + defconfig + 可选 overlay**；**复用** `he200_common.dtsi`，仅 overlay 打开/关闭 PCIe 节点与 EP 驱动，**不要**复制 `he200_peripherals.dtsi` 或 `mmu_regions.c`。

**反例（避免）：**

- 在 `he200_ep/` 下再实现一份 eMMC/GPIO 驱动 — 应已在 `soc/lynxi/ka200` 或公共驱动中完成。
- 用 `CONFIG_BOARD_HE200_EP` 控制 `CONFIG_SDHC_LYNXI_DWCMSHC` — 应 `depends on SOC_LYNXI_KA200` 或 DT。
- 把 `ka200_ep` 当成与 `ka200` 不同的 SoC series — SoC 仍是一个 `lynxi,ka200`，EP 是 SKU 选项。

**现状与后续整理：**

- SoC 启动代码已迁至 `soc/lynxi/ka200/`（`ka200_spl.c`、`ka200_plat.c`、`ka200_early_uart.c`、`ka200_boot_debug.S`、`ka200_smp_shell.c`）；Kconfig 为 `CONFIG_SOC_KA200_BOOT_TRACE`；`he200_ep/` 仅保留 `linker.ld` 与 defconfig。
- PCIe EP 驱动在 Zephyr 中 **尚未移植**；移植时单独目录/Kconfig，不牵动已完成的 SMP/eMMC 路径。

---

## 2. Git 变更总览（建议提交前核对）

在 `zephyr/` 仓库内（分支相对 `origin/main`）：

### 2.1 新增（未跟踪 `??`）

```
boards/lynxi/
  common/he200_common.dtsi      # GIC、UART、SRAM/Flash、8 核 CPU 等
  he200/                        # 基础板：默认无 Shell
  he200_ep/                     # SPL 入口板：Shell、SPL 启动修复、专用 linker
soc/lynxi/
  Kconfig, CMakeLists.txt, soc.yml
  ka200/Kconfig*, mmu_regions.c, CMakeLists.txt
```

### 2.2 修改（已跟踪 `M`）

| 文件 | 变更要点 |
|------|----------|
| `arch/arm64/core/Kconfig` | `NUM_IRQS` 在 `SOC_LYNXI_KA200` 时默认 220 |
| `arch/arm64/core/reset.S` | `CONFIG_SOC_KA200_BOOT_TRACE` 时在 `__start` 调用 `ka200_reset_hook` |
| `arch/arm64/core/prep_c.c` | KA200：`ka200_spl_mmu_prepare()`；可选 prep/mm 跟踪字符 |
| `arch/arm64/core/mmu.c` | 可选 `M`/`T`/`E` mm_init 跟踪字符 |
| `dts/bindings/vendor-prefixes.txt` | 增加 `lynxi` |
| `soc/CMakeLists.txt` | 加入 `lynxi` SoC 子目录 |

### 2.3 建议的 Git 提交拆分

```text
1) soc: lynxi: add ka200 SoC and MMU device regions
   - soc/lynxi/**, vendor-prefixes.txt, soc/CMakeLists.txt
   - arch/arm64/core/Kconfig (NUM_IRQS)

2) boards: lynxi: add he200 and he200_ep board support
   - boards/lynxi/** (不含调试专用源文件可选单独提交)

3) arch: arm64: he200 SPL boot hooks (optional debug)
   - prep_c.c, reset.S, mmu.c 中 CONFIG_HE200_EP_* / SOC_LYNXI_KA200 相关片段
```

生产镜像建议在 `he200_ep_defconfig` 中关闭 `CONFIG_SOC_KA200_BOOT_TRACE`，并视需要去掉 arch 中的跟踪字符（或保留 `he200_ep_spl_mmu_prepare()` 调用，该逻辑不依赖 debug Kconfig）。

---

## 3. 内存与链接布局

与 RT-Thread 一致（`MEM_PADDR_START = 0x800000000`，`_text_offset = 0x100000`）：

| 符号 / 配置 | 地址 |
|-------------|------|
| `CONFIG_SRAM_BASE_ADDRESS` / 代码运行区 | `0x800100000` |
| `CONFIG_FLASH_BASE_ADDRESS` | `0x800900000` |
| SPL / U-Boot 日志中的 entry point | `0x800100000`（ARM64 Image header 起点） |
| `__reset` / `__start`（`he200_ep` linker 优化后） | `0x8001000c4`（紧挨 header 后，见下） |

`he200_ep/linker.ld` 在 `image_header` 之后立即放置 `.text._reset_section`，缩短与 RT-Thread `.text.entrypoint` 的布局差异，避免 SPL 跳入 header 后长距离分支带来的排查困难。

---

## 4. 启动方式：U-Boot SPL → Zephyr（无完整 U-Boot）

### 4.1 流程

```text
ROM/SPL → 从 RAM/eMMC 加载镜像 → Jumping to U-Boot @ 0x800100000
         → 实际为 zephyr.bin（ARM64 Image: b __reset + "ARM\x64"）
         → Zephyr 早期初始化 → MMU → Shell
```

SPL 日志中 `mkimage signature not found` / `ih_magic = 2a0003f4` 属用 **legacy uImage** 解析 **ARM64 Image** 的误报，可忽略；关键是 **`image entry point: 0x800100000`** 与链接地址一致。

### 4.2 烧录

将 `build_*/zephyr/zephyr.bin` 写入原 **u-boot.bin** 分区（与 RT-Thread 相同 SPL 路径）。

### 4.3 编译示例

```bash
west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine
# 产物：build_he200_ep_final/zephyr/zephyr.bin
# he200_ep_defconfig 已默认开启 CONFIG_SMP=8 核 + spin-table + GMAC 网络栈
```

`he200` 板：

```bash
west build -b he200 -d build_he200 app_shell_fs
```

---

## 5. 问题分析与修复（对话调试记录）

### 5.1 现象：SPL 启动无任何打印

| 阶段 | 结论 |
|------|------|
| SPL 加载地址 | 与 RT 相同 `0x800100000`，RT 可启动 → **非 SPL 地址错误** |
| Zephyr 无输出 | SPL **不初始化 UART**；早期代码不能假设 U-Boot 已配置串口 |

**修复：**

- `he200_ep_early_uart.c`：50MHz / 115200 初始化 UART0（对齐 RT `UART_REFERENCE_CLOCK`）
- `he200_ep_spl.c`：`lynxi_sysctl_lite` 等价时钟门（`0x6c/9` fabric_pclk2，`0xb4/1` uart0_sclk）
- `he200_ep_boot_debug.S`：在 `__start` 打印 `S`（需 `CONFIG_SOC_KA200_BOOT_TRACE`）

### 5.2 现象：早期标记 `S2GgL` 后停止

说明已通过 EL2/EL1 平台初始化（`G/g` GIC SRE，`L` EL1 fixup）。

**修复：**

- `he200_ep_spl_el1_fixup()`：关闭过早的 `SCTLR.C`；关闭对齐陷阱位（对齐 RT `init_cpu_sys`）
- `he200_ep_spl_mmu_prepare()`：关闭 SPL 遗留的 **`SCTLR.M`** 并 `tlbi vmalle1`（Zephyr 要求在干净状态下重建页表）

### 5.3 现象：`Ptbd` 后停止（`z_arm64_mm_init`）

| 标记 | 含义 |
|------|------|
| `P` | `soc_prep_hook` / 进入 `z_prep_c` |
| `t`/`b`/`d` | tpidr、bss、data 完成 |
| 无 `m` | 死在 `z_arm64_mm_init()` |

**原因 A — MMU 映射范围过大：**  
曾映射 320MB `SOC_PERIPH`，易耗尽 `CONFIG_MAX_XLAT_TABLES`，`__add_map` 失败但被忽略。

**修复：** 仅映射 `UART0`（`0x10006000`）+ `CPR`（`0x12500000`），`MAX_XLAT_TABLES=16`。

**原因 B — SPL 遗留 EL1 MMU：** 见 5.2 `he200_ep_spl_mmu_prepare()`。

### 5.4 现象：`PtbdDMT` 后停止（`enable_mmu_el1`）

| 标记 | 含义 |
|------|------|
| `D` | 已执行 MMU 准备 |
| `M` | 进入 `z_arm64_mm_init` |
| `T` | `setup_page_tables` 完成 |
| 无 `E` | **开启 MMU 后取指异常** |

**根因（关键）：** 默认 `CONFIG_ARM64_VA_BITS=32` / `PA_BITS=32`，而固件链接在 **`0x800100000`（> 4GB 以下 32 位上限）**。MMU 关闭时按物理地址执行正常；MMU 开启后该 VA 不在 TCR 有效范围内，立即 fault。

**修复（`he200_ep_defconfig`）：**

```ini
CONFIG_ARM64_VA_BITS_40=y
CONFIG_ARM64_PA_BITS_40=y
```

修复后跟踪串：`PtbdDMTEm` → Zephyr banner → Shell。

### 5.5 验证结果

```text
*** Booting Zephyr OS build v4.3.0-... ***
Starting shell example
```

---

## 6. 早期调试字符表（`CONFIG_SOC_KA200_BOOT_TRACE`）

| 字符 | 位置 |
|------|------|
| `S` | `__start`（汇编） |
| `Zephyr SPL` + 行 | `z_arm64_el_highest_plat_init` |
| `2`/`3`/`1` | 当前 EL |
| `G`/`g` | EL2 GIC SRE 前/后 |
| `L` | EL1 plat init |
| `P` | `soc_prep_hook` |
| `t`/`b`/`d` | prep_c 阶段 |
| `D` | MMU 准备后 |
| `M`/`T`/`E` | mm_init 内 |
| `m`/`i` | mm_init 返回、中断初始化后 |

关闭调试：在 `menuconfig` 取消 **Early UART boot stage markers**，或 `CONFIG_SOC_KA200_BOOT_TRACE=n`。

---

## 7. 配置检查清单（he200_ep / SPL）

- [ ] `CONFIG_ARM64_VA_BITS_40` / `CONFIG_ARM64_PA_BITS_40`（必须，除非改为低地址链接）
- [ ] `CONFIG_AARCH64_IMAGE_HEADER=y`（SPL 从 `0x800100000` 执行 header）
- [ ] 烧录 `zephyr.bin` 而非 `zephyr.elf`
- [ ] SPL entry `0x800100000` 与 `CONFIG_SRAM_BASE_ADDRESS` 一致
- [x] DTS `uart0` `clock-frequency = <50000000>`（与 RT 一致；错误时用 24MHz 会导致驱动重配波特率后 Shell 异常）
- [x] `CONFIG_UART_NS16550_DW8250_DW_APB`（DesignWare APB 须用 USR 判断 TX/RX 就绪，否则 `uart:~$` 可能不打印）
- [ ] 生产关闭 `CONFIG_SOC_KA200_BOOT_TRACE`

---

## 8. 与 RT-Thread 差异摘要

| 项目 | RT-Thread HE200 | Zephyr he200_ep |
|------|-----------------|-----------------|
| 入口 | `_start` @ `0x800100000`（`.text.entrypoint`） | ARM64 Image header + `__reset` @ `+0xc4` |
| MMU | `init_mmu_early` 自建页表 | `z_arm64_mm_init` + `mmu_regions.c` |
| 设备映射 | `INTC_BASE` + 320MB DEVICE | GIC + UART + CPR（按需扩展） |
| VA 宽度 | 板级 MMU 描述 64 位物理区 | 必须 **40-bit VA/PA** |
| 时钟 | `lynxi_sysctl_lite_init()` | `he200_ep_spl_soc_init()`（SPL 路径） |

---

## 9. Shell 无 `uart:~$` 提示符

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 有 Zephyr banner / `Starting shell example`，无 `uart:~$` | `device_is_ready(uart0)==0`，Shell `SYS_INIT` 静默失败 | 看 `main` 打印的 `shell uart: ..., ready=0`；查 MMU 是否映射 UART、CPR 时钟门 |
| 有 banner，ready=1，仍无提示符 | 未启用 **DW8250 DW APB**，TX 中断 `irq_tx_ready` 恒为假 | `CONFIG_UART_NS16550_DW8250_DW_APB=y` |
| 有 `uart:~$`，按 Enter 仍无反应 | ① Shell 在 **定时器 ISR** 里 `uart_poll_in` 与 `printk` 抢锁 ② DW UART **FIFO** 与 RT 不一致 | 见下：work 队列轮询、`SOC_LYNXI_KA200` 关 FIFO、`ACCESS_WORD_ONLY` |
| 有 `uart:~$`，键盘无响应 | 未开 **`CONFIG_ARMV8_A_NS`** 时，原生 `intc_gicv3` 把 SPI 配成 Secure G0 且 `irq_enable` 不写 IROUTER | 板级 defconfig 设 `ARMV8_A_NS=y`；**不要**再手写 GIC 寄存器 |
| Shell RX | DTS `gic` + `uart0` + `drivers/interrupt_controller/intc_gicv3.c` + `uart_ns16550` | KA200 无 FIFO 时 `irq_rx_ready` 须看 LSR.DR |

### Shell 自带命令（`CONFIG_KERNEL_SHELL` 等，输入 `help` 查看）

上游 **没有** ARM GICv3 寄存器查看命令（RISC-V 才有 `plic`）。中断相关请用 **`isr_table sw_isr_table`**（向量表，含 hwirq 57 对应 UART ISR）。

| 类别 | 自带命令 |
|------|----------|
| 总览 | `help`、`help -a` |
| 中断 | `isr_table sw_isr_table` |
| 内存/寄存器 | `devmem 0x10006000 16 32`、`kernel heap` |
| 内核 | `kernel version`、`kernel uptime`、`kernel cycles`、`kernel thread list`（含各线程 **CPU%**）、`kernel thread stacks`、`kernel heap` |
| CPU 占用 | **无 `top`**；用 `kernel thread list` 看 `Total execution cycles (N %)`，需 `CONFIG_THREAD_RUNTIME_STATS` |
| 设备 | `device list` |
| 日志 | `log list`、`log enable` |
| 其它 | `date`、`kernel reboot cold`、`kernel panic` |
| 乱码 | DTS 24MHz 与硬件 50MHz 不一致 | 改 `he200_common.dtsi` |

`he200_ep`：`soc_prep_hook()` 内始终调用 `he200_ep_spl_soc_init()`（不依赖 early debug）。

---

## 10. 后续工作（多核与外设路线图）

对照：**RT-Thread** `bsp/lynxi/he200`、**lynxi-linux** `arch/arm64/boot/dts/lynxi/lynchip-lite-base.dtsi`、**lynxi-drivers**（`lyn_drv/drivers/base/dma/`、`sysdma/` 等）。

### 10.1 多核 SMP（spin-table）— **实板已验证**

| 项 | RT / Linux | Zephyr 现状 |
|----|------------|-------------|
| DTS `cpu-release-addr` | 各核均为 `0x0401fff0`（ROM 约定） | `he200_common.dtsi` 已写 |
| 实际 release 单元 | RT `cpu_release_paddr[]`：`0x401ff00` 起每核 +8 | **`soc/lynxi/ka200/pm_cpu_ops_spin_table.c`** |
| MPIDR | `0..3` + `0x100..0x103` | DTS `reg` 与 RT `rt_cpu_mpidr_table` 一致 |
| IPI | GICv3 SGI；跨 cluster 注意 Aff1 | 上游 `arch/arm64/core/smp.c` + `gic_raise_sgi`；异常时对照 RT `gicv3.c` |

**默认镜像（`build_he200_ep_final`）已含 SMP**（`he200_ep_defconfig`：`CONFIG_SMP` + `CONFIG_MP_MAX_NUM_CPUS=8`），无需再叠加 `he200_ep_smp.conf`。

#### 编译与烧录

```bash
west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine
# 烧录 build_he200_ep_final/zephyr/zephyr.bin → 原 u-boot.bin 分区
```

#### 启动日志（实板 2026-06，正常样例）

```text
*** Booting Zephyr OS build v4.3.0-... ***
Secondary CPU core 1 (MPID:0x1) is up
Secondary CPU core 2 (MPID:0x2) is up
Secondary CPU core 3 (MPID:0x3) is up
Secondary CPU core 4 (MPID:0x100) is up
Secondary CPU core 5 (MPID:0x101) is up
Secondary CPU core 6 (MPID:0x102) is up
Secondary CPU core 7 (MPID:0x103) is up
*** he200_ep: SMP=on online_cpus=8 mp_max=8 (shell: he200_smp) ***
uart:~$
```

#### Shell 验收

| 命令 | 期望 |
|------|------|
| `he200_smp` | `arch_num_cpus()=8`，MPIDR 与当前核一致 |
| `kernel thread stacks` | **idle 00～07**、**IRQ 00～07** 各 8 条 |
| `kernel thread list` | 多核 idle 线程存在 |

说明：

- **`CONFIG_LOG_PRINTK` 必须关闭**（`he200_ep_defconfig` 已 `# CONFIG_LOG_PRINTK is not set`）。若开启，printk 进 log 子系统，而 Shell 串口为 `LOG_LEVEL_NONE`，上述启动行在 UART 上**看不见**（但多核仍可能已起来，可用 `kernel thread stacks` 判断）。
- `kernel thread stacks` 中 **IRQ 01～07 显示 100% usage** 多为 secondary 核 ISR 栈检测显示问题，**不一定**表示栈溢出；以 8 个 idle/IRQ 条目存在为准。

### 10.2 外设 DTS（已预置，默认 disabled）

文件：`boards/lynxi/common/he200_peripherals.dtsi`（由 `he200_common.dtsi` include）。

| 外设 | 基址 | SPI | Linux compatible | Zephyr 目标驱动 |
|------|------|-----|------------------|-----------------|
| GPIO | `0x1000e000` | 23 | `snps,dw-apb-gpio` | `gpio_dw` |
| GMAC | `0x10020000` | 78 | `snps,dwmac-*` | `eth_dwmac` / 板级 hook |
| eMMC | `0x10040000` | 80 | `lynxi,dwcmshc-sdhci` | `CONFIG_SDHC_LYNXI_DWCMSHC`（对齐 RT `drv_sdhci.c`，轮询收发） |
| I2C0~3 | `0x10002000`… | 28~31 | `snps,designware-i2c` | `i2c_dw` |
| SPI0 | `0x1000a000` | 32 | `snps,dw-apb-ssi` | `spi_dw` |
| DMA | `0x1001a000` | 83 | `snps,axi-dma-1.01a` | `dma_dw_axi` |

MMU：`soc/lynxi/ka200/mmu_regions.c` 已增加 **SPIN_TABLE** + **SOC_APB**（`0x10002000`，256KB）+ **EMMC**（`0x10040000`，256KB）。

**统一构建目录**（SMP、Shell、eMMC 及后续外设均编入同一镜像）：

```bash
west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine
# 产物：build_he200_ep_final/zephyr/zephyr.bin
```

`he200_ep_defconfig` 已启用 `CONFIG_SDHC_LYNXI_DWCMSHC`；`he200_peripherals.dtsi` 中 `emmc0` 为 `status = "okay"`。`app_shell_fs` 负责启动探测与 FatFS 挂载。磁盘 `SD2`，挂载点 `/SD2:`。FAT 自动挂载由 `CONFIG_APP_HE200_EMMC_AUTO_MOUNT` 控制（默认关闭，init 与挂载分步验收）。

### 10.3 驱动实现顺序（建议）

1. **CPR 时钟门控** — `soc/lynxi/ka200/sysctl_lite.c`（对齐 RT `drv_sysctl_lite.c`：eMMC `0x88`、GMAC `0x8c`、I2C `0x94/0x98`、DMA `0x6c`）
2. **CPR IP 复位** — RT `drv_reset.c`（ETH/I2C/RTC/DMA 脉冲）
3. **GPIO** → pinmux / 中断（Linux `lynxi_pinfun` + lynxi-drivers）
4. **I2C** → 传感器 / PMIC
5. **DMA** — mem2mem（RT `drv_dw_axi_dma.c`；lynxi-drivers `lynd_dma.c`）
6. **eMMC** — [已验证] `sdhc_lynxi_dwcmshc.c` + `sysctl_lite.c`（CPR `0x88` gate）；RT 式 MMC init + PIO 数据路径；实板 `disk_access_init` 通过（§10.5）
7. **GMAC** — [ping/UDP 已验] `eth_dwmac_lynxi_ka200.c` + MDIO + RTL8211F；TCP ~0.07 Mbps（§10.6）
8. **SPI / SFC** — Linux 含 `lynxi,spi-sfc` 与 AHB boot SPI，后期单独板级

### 10.4 待办 checklist

- [ ] `he200` 板启用 Shell 并做 SPL 启动验证（当前 defconfig 关闭 UART/Shell）
- [x] spin-table `pm_cpu_on` + `he200_ep_defconfig` 默认 SMP
- [x] `he200_peripherals.dtsi` + MMU SOC_APB / SPIN_TABLE / EMMC
- [x] SMP 实板 8 核启动（spin-table + 启动日志 + `he200_smp` / `kernel thread stacks`）
- [ ] SMP IPI 调度压测（对照 RT README §5 IPI，跨 cluster 异常时再查）
- [x] eMMC 编入 `build_he200_ep_final` + `app_shell_fs`（实板 `disk_access_init` 已验）
- [ ] eMMC FAT 自动挂载（`CONFIG_APP_HE200_EMMC_AUTO_MOUNT=y`）与 `fs ls /SD2:` 读写压测
- [x] GMAC init + MDIO + PHY ID（`he200_ep_defconfig` 实板 2026-06）
- [x] GMAC 链路 up / ping 8/8（2026-06-08，`he200_gmac_ping_test.sh`）
- [x] GMAC UDP 带宽 ~52 Mbps（Host→EP，iperf v2 + zperf）
- [ ] GMAC TCP 带宽优化（当前 ~0.07 Mbps，疑协议栈；`CONFIG_ZVFS_POLL_MAX=9` 已修 zperf server）
- [ ] GMAC EP→Host TCP（`zperf tcp upload` 待查）
- [ ] GPIO / I2C / SPI / DMA 驱动与 `status = "okay"`
- [x] `README.md` He200 章节（编译、SMP、eMMC 验收）
- [ ] 若新增 `he200_rc` 板：复用 `he200_common.dtsi` + 独立 `defconfig`/linker 即可

### 10.5 eMMC 实板验证（2026-06）

对照 RT-Thread `bsp/lynxi/he200/drivers/drv_sdhci.c` 与 Linux `lynxi,dwcmshc-sdhci`，在 `he200_ep` + `app_shell_fs` 上完成 eMMC 初始化与几何信息读取。

#### 验收日志（关键片段）

```text
he200 eMMC: init disk SD2
disk_access_init returned 0
disk_access_init OK
geometry: sectors=61071360 size=512 (~29820 MB)
FS mount skipped (CONFIG_APP_HE200_EMMC_AUTO_MOUNT=n)
```

完整 init 序列含：CMD0/CMD1 probe、CID（实板 `4a544434` = "JTTD"）、CSD、SELECT、8-bit SWITCH、EXT_CSD 512B、HS timing SWITCH。

#### 主要修复点（DWC MSHC + Zephyr SD 子系统）

| 问题 | 现象 | 修复 |
|------|------|------|
| 虚假 Command Complete | CMD2 读到 CMD1 OCR 残留 `ff808000`，`disk_access_init -134` | `CARD_IS_EMMC` 在 phy_init 早期设置；R3 用 raw RESP；RT 式 `CMD0→CMD1(probe)→CMD0→CMD1(ocr\|HCS)→CMD2`；stale RESP 检查**仅 CMD2** |
| R1b 后 CMD13 失败 | SWITCH 后 `sdmmc_wait_ready` 读到旧 RESP | stale 检查不用于 CMD13；R1b 后 `lynxi_wait_dat0_ready()` + `lynxi_card_busy()` |
| EXT_CSD 读挂死 | CMD8 数据阶段无 `DATA_AVAIL` | `lynxi_cmd_data_is_read()` 覆盖非写块命令；数据阶段关 IRQ、按 `PRESENT_STATE` PIO 轮询（对齐 RT） |

### 10.6 GMAC 移植进度（2026-06）

**唯一参考：lynxi-linux**（`arch/arm64/boot/dts/lynxi/` + `drivers/net/ethernet/stmicro/stmmac/` + `drivers/gpio/gpio-dwapb.c` + `drivers/pinctrl/pinctrl-lynlite.c` + `drivers/clk/lynxi/clk-lynxi-lite.c`）。RT-Thread GMAC 不作对照。

**实板验收（2026-06）**：`POST_KERNEL` 全流程通过；`PHY ID 001c:c916`（RTL8211F，对照 `lynxi-linux/drivers/net/phy/realtek.c`）；eMMC 与 GMAC 可并存启动。

**代码更新（2026-06-06）**：

- `phy_mii.c`：RTL8211F `rgmii-id` TX/RX delay（对齐 `realtek.c`）
- `he200_ep_defconfig`：`CONFIG_NET_SHELL` + 静态 IP `192.168.1.2/24`（`he200_ep_gmac.conf` 仅作可选覆盖）
- `eth_dwmac.c` / `eth_dwmac_lynxi_ka200.c`：RGMII 线中断 W1C + 屏蔽，防 IRQ 风暴

链路 up / ping 仍待 `192.168.49.81` 实板复验（Host `enp25s0f1` → `192.168.1.1/24`；**ping 前**再跑 RT 同目录 `he200_test_env.sh` 并确认 `ip -4 addr show enp25s0f1` 无 `49.81`）。

**2026-06-06 对照**：`build_he200_ep_final/zephyr.bin` 热插拔后 probe/PHY 正常，Host `ping 192.168.1.2` 仍无 ARP Reply。RT 侧根因已定位为 **MTL TSF/RSF + RXQ0 DCB**（`lynxi_dwmac4_mtl_init`）；Zephyr 已在 `eth_dwmac_lynxi_ka200.c` 合入同等 `lynxi_dwmac_mtl_init()`。验收脚本 Zephyr 分支 `BOOT_WAIT=50`（勿被顶层默认 22s 覆盖）。

**2026-06-08 实板 ping PASS**（`FIRMWARE=zephyr he200_gmac_ping_test.sh` → 8/8，0% 丢包）：

| 项 | 修复 |
|----|------|
| MTL | `lynxi_dwmac_mtl_init()`：RXQ0 DCB + TSF/TXQEN/RSF |
| MMU PA | `eth_dwmac.c`：`k_mem_phys_addr()` 写 DMA des0/des1 |
| RX cache | 收包路径 `sys_cache_data_invd_range()` |
| RX 环启动 | iface init 等待 refill 提交 N-1 描述符后再启 DMA |
| GIC IRQ | SPI 110（GMAC）改 **level-active-high**（Zephyr 默认 active-low） |
| PHY carrier | BMSR 兜底 + `dwmac_service()` 10ms 轮询兜底 |
| 缓冲 | `CONFIG_NET_BUF_DATA_SIZE=1518`，`DWMAC_NB_RX_DESCS=32` |
| 验收脚本 | Zephyr 分支 `BOOT_WAIT=50` |

构建：`west build -b he200_ep -d build_he200_ep_final app_shell_fs --pristine`（GMAC 已编入 `he200_ep_defconfig`）

**2026-06-08 实板带宽（49.81，`FIRMWARE=zephyr`）**：

| 方向 / 协议 | 工具 | 结果 | 说明 |
|-------------|------|------|------|
| Host → EP | UDP iperf v2 + `zperf udp download` | **~52 Mbps** | 与 RT 同量级，GMAC DMA 正常 |
| EP → Host | `zperf udp upload` + Host `iperf -s -u` | **~50 Mbps** | zperf 配置上限 50M |
| Host → EP | TCP iperf v2 + `zperf tcp download` | **~0.07 Mbps** | 与 RT 同量级，疑 TCP 栈 |
| EP → Host | `zperf tcp upload` | **N/A** | Host `iperf -s` 无连接统计，待查 |

验收脚本（RT 仓库 `bsp/lynxi/he200/scripts/`）：`he200_gmac_ping_test.sh`、`he200_gmac_bw_test.sh`、`he200_gmac_tcp_test.sh`（iperf **v2、端口 5001**；Zephyr `BOOT_WAIT=50`）。

**串口可见性**：`he200_ep` 关闭 `CONFIG_LOG_PRINTK`；驱动内 `LOG_INF` 不出 UART。验收读 `printk`：`he200 GMAC:` / `he200 PHY:`（`app_shell_fs/src/main.c` 亦有 `he200 GMAC: probe phy@1`）。

#### 调试阻塞点（实板演进，按时间序）

| # | 最后可见日志 / 位置 | 根因（对照 lynxi-linux） | 处理 |
|---|---------------------|--------------------------|------|
| 1 | `he200 PHY: pinctrl PD23 gpio`（`gpio_dw`） | Lynxi Lite **无** `SWPORTx_CTL`；访问 `0x1000e02c` 挂死。Linux `gpio-dwapb` 亦不用 CTL，复用由 `pinctrl-lynlite` bit10 控制 | KA200 下 `gpio_dw_config()` 对 PD23 跳过 `gpio_dw_set_hw_mode()`，改 `lynxi_pinctrl_pin_gpio_mode(118)` |
| 2 | `bus init start` 后无输出（`lynxi_phy_reset` @ `bus_init`） | 误加 `lynxi_sysctl_gpio_reset_pulse()`（CPR `0x90` bit0）。Linux `lynchip-lite-base.dtsi` 的 `gpio@1000e000` **无** `resets`，`gpio-dwapb` 仅 `reset_control_deassert(optional NULL)` | 删除 GPIO CPR bit0 脉冲；`lynxi_sysctl_gpio_enable()` 只开 bit1/2（`clk-lynxi-lite.c` `LITE_PERIPH_GPIO_DB/INTR`） |
| 3 | `gpio pulse portd:23`（直接 MMIO `*ddr=BIT(23)`） | **整口写** `SWPORTD_DDR` 把 PD10–22（RGMII0）方向清零；GMAC/RGMII 已开时总线挂死。Linux `bgpio` 对 DDR 做 **RMW** 只改目标 bit | 改为 `\|=` / `&=` RMW；仍不足（见 #4） |
| 4 | `gpio pulse portd:23 rmw` / `gpio-dwapb reset portd:23` | `phy_ref`（CPR `0x8c` bit9）使能后，Port-D `SWPORTD_DDR` 读（`bgpio` RMW 必经）在 Zephyr 实板挂死。Linux 在完整 `gpiolib`+`pinctrl` 下 `mdiobus_register_gpiod()` 可工作；Zephyr 裸机路径等效访问仍挂 | **KA200 跳过** `reset-gpios` 的 `gpio-dwapb` 硬复位，改 **BMCR 软复位**（`phy_mii`）；MDIO 已能读 ID 时等价于 Linux 无 `reset-gpios` 的 generic PHY 路径 |
| 5 | （若误写）CPR `0x88` bit0 | `LITE_EMMC_R`（eMMC IP 复位），与 eMMC 驱动冲突 | **禁止**在 GMAC 路径写 `0x88` bit0 |
| 6 | `DMA SWR timeout` | `DMA_MODE` bit0 粘住（`0x1`）；Linux `dwmac4_dma_reset()` 10×10ms 后返回 `-EBUSY` 并中止 probe | 检测逻辑对齐 Linux；KA200 打 `LOG_WRN` **继续** init（MAC/MDIO 仍可用，属板级 quirk，非 Linux 行为） |
| 7 | `POST_KERNEL start` 后无 `Booting Zephyr OS` | 卡在 eth(55)→mdio(56)→phy(57) 某一 init；用最后一条 `he200 GMAC:` / `he200 PHY:` 定位 | 见下「init 顺序」 |

**init 顺序（Linux 等价）**：`PRE_KERNEL` GPIO 时钟 → `eth@55` `dwc_qos_probe` 时钟 → `mdio@56` MDIO 探测 → `phy@57` 复位+自协商 → `boot_banner`。GIC IRQ 在 `dwmac_iface_init` 才 `irq_enable`。

**成功启动标志**：

```text
he200 GMAC: mdio PHYID1 probe addr=1 val=0x001c ret=0
he200 PHY: BMCR soft reset (MDIO PHYID ok, skip gpio-dwapb)
*** Booting Zephyr OS build ...
he200 GMAC: PHY ID 001c:c916 (0x001cc916)
```

#### Linux 对照表（HE200 EVB）

| 项 | lynxi-linux | Zephyr 实现 | 对齐 |
|----|-------------|-------------|------|
| DTS `&eth` | `ethernet@10020000`，`clock-names` aclk/phy_ref | `he200_peripherals.dtsi` `eth` | ✓ |
| EVB PHY | `phy-mode=rgmii-id`，`reset-gpios=<&portd 23>` | `phy-connection-type=rgmii`（无 rgmii-id 枚举），DTS 仍保留 reset-gpios | △ 模式枚举差 |
| 时钟 probe | `dwc_qos_probe()`：aclk → phy_ref_clk；**不写** `0x66f` | `lynxi_sysctl_gmac_probe_clocks()` gate bit1/9/2 + `LITE_GMAC_R` 脉冲 | ✓ |
| CPR 速率 | `lynchip_lite_cpr_gmac_config()` **链路建立后**写 `0x66f/0x65f/0x64f` | `lynxi_dwmac_apply_link_speed()` 回调里 `lynxi_sysctl_gmac_cpr_speed_set()` | ✓ |
| DMA 复位 | `dwmac4_dma_reset()` 10×10ms，失败 `-EBUSY` | 同循环；KA200 SWR 粘住时 **WARN 继续** | △ 失败策略 |
| MDIO | `snps,dwmac-mdio`，GMAC4 @ `0x200`，`csr-clock-range` | `mdio_lynxi_dwmac.c` | ✓ 实板 ID `0x001c` |
| PHY 硬复位 | `mdiobus_register_gpiod()` + `gpio-dwapb` + `pinctrl` PD23 | KA200：**跳过 gpio**，`MII_BMCR_RESET` 软复位 | ✗ 见 #4；功能上 SPL+MDIO 已通 |
| PHY 驱动 | `realtek.c` RTL8211F 专用 | `phy_mii` generic | △ 待换 `phy_realtek` 可选 |
| GPIO CPR | `0x90` bit1/2 only；无 GPIO block reset | `lynxi_sysctl_gpio_enable()` 同 | ✓ |
| eMMC CPR | 勿动 `0x88` bit0 | 文档约束 + 代码未写 | ✓ |

#### 已知遗留与 Linux 是否对齐

| 遗留项 | Linux 行为 | Zephyr 现状 | 是否对齐 |
|--------|------------|-------------|----------|
| Port-D `reset-gpios` 硬复位 | EVB DTS 有；`mdiobus_register_gpiod()` 经 `gpio-dwapb` 执行 | 实板 DDR RMW 挂死，改 BMCR 软复位 | **否**（有意偏离，见 #4） |
| DMA SWR 粘住 | probe 失败 `-EBUSY` | WARN 后继续，MDIO/MAC 可用 | **否**（板级 quirk 容错） |
| `phy-mode` rgmii-id | EVB 使用 | Zephyr 仅 `rgmii` | **否**（框架限制） |
| probe 写 `0x66f` | 不写，链路后写 | `gmac_probe_clocks()` 不写；链路回调写 | **是** |
| `0x8c` 时钟门控顺序 | aclk → phy_ref → hclk | 同 | **是** |
| CPR `0x90` GPIO reset 脉冲 | 无（DTS 无 resets） | 已删除 | **是** |
| RTL8211F 识别 | `CONFIG_REALTEK_PHY` | generic `phy_mii`，ID 字符串 hint | **功能等价**，驱动未专用化 |

#### 阶段状态

| 阶段 | 状态 | 说明 |
|------|------|------|
| CPR / 时钟 | ✓ | `lynxi_sysctl_gmac_probe_clocks()`，probe 阶段 `0x8c≈0x207` |
| MAC / DMA | △ | `MAC_VERSION=0x152`；SWR 超时但继续 |
| MDIO | ✓ | `PHYID1=0x001c` @ addr 1 |
| PHY init | ✓ | BMCR 软复位；`0x001cc916` RTL8211F |
| 链路 / ping | ✓ | 8/8 PASS；`net iface`、carrier on |
| UDP 带宽 | ✓ | Host→EP ~52 Mbps；EP→Host ~50 Mbps |
| TCP 带宽 | △ | Host→EP ~0.07 Mbps；EP→Host 待查 |
| 专用 PHY 驱动 | 可选 | 可启用 `CONFIG_PHY_REALTEK` 对照 Linux |

#### 相关源文件

| 路径 | 作用 |
|------|------|
| `drivers/ethernet/eth_dwmac_lynxi_ka200.c` | bus_init 时钟、链路 CPR 回调 |
| `drivers/ethernet/eth_dwmac.c` | DMA SWR（`dwmac4_dma_reset` 时序） |
| `drivers/ethernet/mdio/mdio_lynxi_dwmac.c` | DWMAC4 MDIO @ `0x200` |
| `drivers/ethernet/phy/phy_mii.c` | KA200 BMCR 软复位（跳过 gpio-dwapb） |
| `drivers/gpio/gpio_dw.c` | KA200 PD23：pinctrl + 无 CTL |
| `soc/lynxi/ka200/sysctl_lite.c` | GMAC/GPIO CPR、pinctrl helper |
| `soc/lynxi/ka200/mmu_regions.c` | GMAC `0x10020000`、SOC_APB（含 GPIO） |
| `boards/lynxi/common/he200_peripherals.dtsi` | `&eth`、`ethernet-phy@1` |
| `boards/lynxi/he200_ep/he200_ep_defconfig` | 默认 SMP + eMMC + GMAC 网络栈 |
| `boards/lynxi/he200_ep/he200_ep_gmac.conf` | 可选覆盖（默认已编入 defconfig） |

---

## 11. 关键文件索引

| 路径 | 作用 |
|------|------|
| `soc/lynxi/ka200/ka200_spl.c` | SPL 时钟、EL1/MMU 准备、plat_init |
| `soc/lynxi/ka200/ka200_plat.c` | EL2 GIC SRE |
| `boards/lynxi/he200_ep/linker.ld` | header 后放置 reset 段 |
| `soc/lynxi/ka200/mmu_regions.c` | GIC / SPIN_TABLE / UART / CPR / SOC_APB / EMMC MMU |
| `soc/lynxi/ka200/sysctl_lite.c` | CPR 时钟门控（eMMC `0x88` 等） |
| `soc/lynxi/ka200/pm_cpu_ops_spin_table.c` | 无 PSCI 时 `pm_cpu_on()`，RT release 地址表 |
| `soc/lynxi/ka200/ka200_smp_shell.c` | Shell `ka200_smp`（别名 `he200_smp`） |
| `drivers/sdhc/sdhc_lynxi_dwcmshc.c` | Lynxi DWC MSHC SDHCI 主机驱动 |
| `subsys/sd/mmc.c` / `subsys/sd/sd.c` | MMC/eMMC 初始化协议（对齐 RT） |
| `arch/arm64/core/prep_c.c` | `ka200_spl_mmu_prepare()` 调用点 |

文档版本：SPL + Shell + **8 核 SMP** + **eMMC init** + **GMAC/MDIO/PHY init 实板验证**（Zephyr `v4.3.0` / `zephyr_ka200` 分支）。
