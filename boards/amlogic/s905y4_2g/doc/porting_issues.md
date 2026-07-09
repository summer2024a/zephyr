# 移植问题与解决方案

Board: `s905y4_2g` / SoC: `meson_s4` (S905W2/S905Y4)

实板调试记录，按问题分类。调试方法见 [debug.md](debug.md)，启动流程见 [zephyr_boot_flow.md](zephyr_boot_flow.md)。

---

## 1. UART TX 缓冲同步（主因）

### 现象

```text
Zephyr SPL (S4)
HGgLP
```
之后乱码或完全 hang。

### 根因

Meson UART 连续写 WFIFO 时只等待 TX_FULL 清零，未等待 TX_EMPTY。TX FIFO 积累未发送字符，导致 marker 乱码或后续访问 hang。

### 寄存器

| 偏移 | 名称 | 关键位 |
|------|------|--------|
| +0x00 | WFIFO | 写字符 |
| +0x0C | STATUS | bit 21 TX_FULL, bit 22 TX_EMPTY |

### 修复

`meson_s4_early_uart.c` / `uart_meson.c` 的 `poll_out`：

```c
*s4_uart_wfifo = (uint32_t)c;
while ((*s4_uart_status & TX_EMPTY_BIT) == 0U) { ; }
```

### 验证

| 版本 | Boot markers |
|------|-------------|
| 原始 | `HGgLP` + 乱码 |
| 仅 TX_EMPTY | `HGgLP12345`（推进到 MMU 前） |
| TX_EMPTY + cache 修复 | `HGgLP12345abCdefg...` |

---

## 2. Cache 一致性（辅助，与 UART 修复配合）

### 现象

- 仅修 UART：推进到 `12345` 后 hang
- 仅修 cache：仍 `HGgLP` 乱码

### 根因

U-Boot standalone `bootm` **不调用** `cleanup_before_linux()`，BL31/U-Boot 遗留 dirty dcache lines。MMU+dcache 同时开启后，页表遍历或 UART 访问可能读到 stale cache。

### 分层修复

| 阶段 | 位置 | 操作 |
|------|------|------|
| EL2 入口 | `reset.c` `z_arm64_el2_init()` | `CONFIG_ARM64_BOOT_DISABLE_DCACHE`：flush + 关 dcache |
| EL1 plat | `meson_s4_plat.c` | `arch_dcache_flush_and_invd_all()` 后清 SCTLR.C/M |
| MMU 使能 | `mmu.c` `enable_mmu_el1()` | flush + 先开 MMU 后开 dcache（S4 延迟 dcache） |
| 内核入口 | `kernel/init.c` `z_cstart()` | `meson_s4_enable_dcache_el1()` |

### Kconfig

```
CONFIG_ARM64_BOOT_DISABLE_DCACHE=y
CONFIG_ARM64_DCACHE_ALL_OPS=y
```

### S4 MMU 额外策略

- TCR PTW 设为 Non-Cacheable（`TCR_IRGN_NC | TCR_ORGN_NC`）
- `enable_mmu_el1()` 中 S4 不立即开 dcache，延迟到 `z_cstart()`

### SMP 路径额外注意

单核策略不能直接套用到 PSCI 热启动从核，详见 [smp_psci_boot.md](smp_psci_boot.md) §3.3–3.4。

| 角色 | dcache 开启时机 | 与 Linux 差异 |
|------|----------------|--------------|
| CPU0 主核 | `z_cstart()` → `meson_s4_enable_dcache_el1()` | Linux 主核在 `__enable_mmu` 即开 C bit |
| CPU1~3 从核 | `soc_per_core_init_hook()`（在清 `fn` 前） | Linux 从核 `secondary_entry` → `__enable_mmu` 同步开 C bit |
| 共享变量 `arm64_cpu_boot_params.fn` | 从核 flush + 主核 invd（`arch/arm64/core/smp.c`） | Linux 用 `secondary_data` + `dsb(ishst)` |

**风险**：主核 dcache ON 时 `wfe` 轮询 `fn`，从核写 `fn = NULL` 须 flush/invalidate 保持一致。已加 generic cache 维护 + `soc_per_core_init_hook()`。

---

## 3. PRE_KERNEL_1 UART 驱动 hang

### 现象

Boot markers 推进到 `k`（`board_early_init_hook` 完成）后 hang，trace 显示卡在 `meson_uart_init()`（marker `p`/`r`/`v`/`w`）。

### 根因

`meson_uart_init()` 默认调用 `DEVICE_MMIO_MAP()` → `k_mem_map_phys_bare()`，与 S4 静态 identity map 冲突；即使跳过 map，PRE_KERNEL_1 内对 UART 寄存器的 `sys_read32/write32` 仍 hang。

### 调用链

```text
meson_uart_init()  [PRE_KERNEL_1]
  └─ DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE)
       └─ device_map()
            └─ k_mem_map_phys_bare()
                 ├─ virt_region_alloc()    # 分配内核 VA（非 identity）
                 └─ arch_mem_map()         # 写页表 + TLB shootdown
```

### 为何 k_mem_map_phys_bare 不可用

S4 在 `mmu_regions.c` 已用 `MT_NO_OVERWRITE` 建立 APB4 identity map（UART @ `0xFE07A000`）。当前 defconfig **未启用 `CONFIG_KERNEL_DIRECT_MAP`**。

| 路径 | 结果 |
|------|------|
| 默认（virt_region_alloc 新 VA） | 冗余双映射；PRE_KERNEL_1 时 `invalidate_tlb_all()` 实板 hang |
| DIRECT_MAP（VA=PA） | PTE 已被 static map 占用 → `entry already in use` → k_panic |
| 跳过 map，直接读寄存器 | PRE_KERNEL_1 仍 hang |

Early UART 已在 EL1 阶段完成硬件 init，PRE_KERNEL_1 不应再 poke UART 寄存器。

### 修复（已验证）

`drivers/serial/uart_meson.c` S4 专用：

1. `meson_uart_init()`：直接 `return 0`，跳过 `DEVICE_MMIO_MAP` 及寄存器配置
2. `meson_uart_read/write()`：固定 `S4_UART_B_PHYS (0xFE07A000)`
3. `poll_out()`：保留 TX_EMPTY 等待

实板结果：`Hello World! s905y4_2g/meson_s4`

### 长期可选方案

| 方案 | 说明 |
|------|------|
| SoC 级 identity MMIO | 已在 static map 的外设不走 `DEVICE_MMIO_IS_IN_RAM` |
| 延后 init | UART 寄存器配置移到 POST_KERNEL |
| device_map 感知静态映射 | PA 已有 identity map 时 `*virt_ptr = phys`，跳过 k_mem_map |

---

## 4. MMU 页表 stale cache

### 现象

单核调试 marker 序列 `pP1234a` 后 hang（`a` = MMU init 返回，`5` 之后无输出）。

### 根因

`z_arm64_mm_init()` 成功后 MMU+dcache 开启，但 boot marker 写 UART 时访问 device 区域触发 Data Abort 或 hang。

### 修复

`enable_mmu_el1()` 中 S4 专用：

- MMU 使能前 `arch_dcache_flush_and_invd_all()`
- 先开 MMU（dcache off），dcache 延迟到 `z_cstart()`

---

## 5. SMP 投票锁

### 现象

SMP 启用时 boot marker 从 `LP` 变为 `HGgLP`，但单核仍 hang。

### 分析

`arm64_cpu_boot_params.voting[]` 在 .bss 清零前可能有垃圾数据。已在 `reset.S` 中投票锁之前手动清零。

### 调试建议

单核调试时设 `CONFIG_SMP=n`，排除多核干扰。

---

## 6. SECMON 内存区域

### 约束

`0x05000000 ~ 0x08200000` 为 BL31 Secure Monitor 区域，Non-secure 访问会 fault。

DTS 和 MMU 映射须避开此区域。Zephyr 代码/数据从 `0x01000000` 起，不重叠。

---

## 7. PSCI 早期 SMC

### 现象

PRE_KERNEL_1 中 PSCI_VERSION SMC 可能导致早期 hang。

### 处理

可 defer PSCI init 到 POST_KERNEL，或在单核调试时暂时禁用 SMP/PSCI 相关 init。

---

## 10. Meson UART 中断驱动 + Shell

### 10.1 现象汇总（2026-07-09 更新）

| 模式 | TX | RX | 结果 |
|------|----|----|------|
| **irq-full Shell**（**defconfig 当前**） | 中断 | 中断 | **基本正常**；SMP 用 `SMP_AUTO_PROBE`，Shell 勿首条发 `meson_s4_gic` |
| **混合**（`overlay-irq-rx.conf`） | 轮询 | 中断 | **稳定** |
| **全轮询**（`overlay-poll-uart.conf`） | 轮询 | 轮询 | **稳定**，SMP 验收备选 |

Tick/GIC **PPI**（Arch Timer）已用 `kernel uptime` 验证正常。

### 10.2 GIC SPI 169 / CPU Interface（2026-07-09 实板 `meson_s4_gic`）

| 项 | Linux | Zephyr |
|----|-------|--------|
| DTS SPI 编号 | `GIC_SPI 169` | 同左 |
| GIC INTID / Zephyr IRQ | 32+169 = **201** | `DT_IRQN(uart_b)` = **201** |
| ISR 向量 | — | `isr_tables.c` irq **201** → `meson_uart_isr` |
| GICD enable | — | irq 201 **enabled=1** |
| CPU IF | `GICC_ENABLE` | `GICC_CTLR=0x41`（GRP0+GRP1），`PMR=0xf0` |

**易错点**：用 Linux SPI **169** 读 GIC 寄存器会查到错误线路（enabled=0）；须用 Zephyr **201** 或 `arch_irq_is_enabled(201)`。

Shell 命令：`meson_s4_gic` / `s4_gic`（需 `CONFIG_SOC_MESON_S4_SHELL_SMP=y`）。

### 10.3 RX/TX 中断根因与修复

1. **`IRQ_CONNECT(..., 0)` 未传 DTS flags** → GIC ICFGR 保持 `gic_dist_init` 的 level；改为 `DT_INST_IRQ(0, flags)` 后 **RX/TX 中断 Shell 均恢复**（主因）。
2. **原驱动 `TX_INT_EN`/`RX_INT_EN` 门控、`!TX_FULL` 判断**（对齐 Linux，见 §10.4）。
3. **Shell `tx_busy` + 8B TX ring**：纯 TX 中断模式依赖后续 GIC SPI；修复 flags 后 prompt 可正常出现。
4. **PRE_KERNEL 不可 reset FIFO/REG5** — POST_KERNEL 仅 `meson_s4_uart_irq_prepare()`。

### 10.4 当前驱动（`drivers/serial/uart_meson.c`）

- `irq_tx_ready` / `irq_rx_ready`：**INT_EN 置位** 且 `!TX_FULL` / `!RX_EMPTY`（对齐 Linux）
- `irq_is_pending()`：委托上述 ready 函数
- ISR：仅在 `irq_is_pending()` 时 kick callback
- RX 读路径：**CLR_ERR** 清帧/parity 错误
- S4：**POST_KERNEL** `meson_s4_uart_irq_prepare()` + `meson_s4_gic_force_edge_rising()`
- **TX 流控对齐 Linux**：`start_tx`/`stop_tx`（burst + 按需 `TX_INT_EN`），ISR 单次 callback
- **`IRQ_CONNECT(..., DT_INST_IRQ(0, flags))`** 传递 DTS `IRQ_TYPE_EDGE`

### 10.5 Overlay 用法

板级目录下两个 Kconfig 片段（**叠加**在 `s905y4_2g_defconfig` 之上）：

| 文件 | 作用 |
|------|------|
| `overlay-irq-rx.conf` | Shell **RX 中断 + TX 轮询**，测 GIC/UART SPI 201 |
| `overlay-poll-uart.conf` | Shell **全轮询**（从 irq-full 切回轮询验收） |

**Defconfig（2026-07-09）**：`CONFIG_SHELL_BACKEND_SERIAL_INTERRUPT_DRIVEN=y` + `CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE=y`（main 里 `z_smp_init`，4 核）+ TX ring 256。

**稳定性测试**：

```bash
./board_test.sh stability          # 连续 3 次启动，2/3 PASS（irq-full）
STABILITY_RUNS=5 ./board_test.sh stability
```

**方式 A — `board_test.sh`**

```bash
cd zephyr/boards/amlogic/s905y4_2g

# 默认 irq-full + SMP auto-probe
./board_test.sh boot

# 切回全轮询
OVERLAY=poll-uart ./board_test.sh boot

# 混合 RX 中断
OVERLAY=irq-rx ./board_test.sh boot
```

**方式 B — `west build` 直接指定**

```bash
cd /work/zephyr-rtos/zephyrproject && source zephyr/zephyr-env.sh

west build -b s905y4_2g -d build_s4_shell -s zephyr/samples/hello_world --pristine -- \
  -DEXTRA_CONF_FILE=zephyr/boards/amlogic/s905y4_2g/overlay-irq-rx.conf
```

上板后在 `uart:~$` 执行 `meson_s4_gic`：关注 `zephyr_irq=201`、`RX_INT_EN=1`、`meson_uart_isr_count` 是否随按键/命令增长。

```kconfig
# defconfig 当前
CONFIG_SHELL_BACKEND_SERIAL_INTERRUPT_DRIVEN=y
CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE=y
CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE=256
```

### 10.6 验证记录

| 日期 | 配置 | 结果 |
|------|------|------|
| 2026-07-08 | 全轮询 + SMP | `meson_s4_smp` → 4 核 OK |
| 2026-07-08 | 全轮询 + `help` | **help / kernel version 有回显** |
| 2026-07-08 | 混合 IRQ-RX（flags=0） | prompt OK，**命令无回显** |
| 2026-07-08 | 纯 IRQ TX/RX（flags=0） | 无 prompt |
| 2026-07-08 | `kernel uptime`×2 | tick +400ms，GIC PPI OK |
| 2026-07-09 | GIC 诊断 `meson_s4_gic` | irq 201 enabled=1；此前误查 irq 169 |
| 2026-07-09 | 混合 IRQ-RX + flags fix | **help / kernel version OK**，isr_count 增长 |
| 2026-07-09 | defconfig irq-full + SMP auto-probe | stability **2/3 PASS**；help 先于 meson_s4_gic |
| 2026-07-09 | irq-full + Linux 对齐 `start_tx`/`stop_tx` | stability **2/3 PASS**；Run3 hang 在 `z_smp_init()`，非 Shell |
| 2026-07-09 | SMP 同步加固（dcache/flush/wfe/sev/PSCI retry） | stability **5/5 PASS** |

**Linux 对齐 TX 流（`uart_meson.c`）**

与 Linux `meson_uart.c` 一致：

- `irq_tx_enable` → `meson_uart_start_tx()`：先 burst 回调填 FIFO，剩余数据再开 `TX_INT_EN`
- `irq_tx_disable` → `meson_uart_stop_tx()`：仅清 `TX_INT_EN`，保留 `TX_EN`/`RX_INT_EN`
- ISR：RX 或 TX ready 时单次 callback（同 Linux `meson_uart_interrupt`）

### 10.7 SMP 启动偶发 hang 修复（2026-07-09）

**现象**：irq-full + `SMP_AUTO_PROBE` 稳定性 **2/3**，失败 run hang 在 `z_smp_init()` / `arch_cpu_start` 的 `wfe` 等 `fn=NULL`。

**根因**（主从不对称 cache）：

1. 从核 PSCI 唤醒时 BL31 可能仍开着 dcache，读 `boot_params.mpid` 命中 stale line → 卡在 `reset.S` `secondary_core`
2. 从核 MMU 后 dcache 仍 OFF，主核 dcache ON 轮询 `fn` → 偶发看不到 `fn=NULL`
3. Hello 后立即 `z_smp_init()`，GIC/UART IRQ 与 PSCI 竞态

**修复**：

| 位置 | 改动 |
|------|------|
| `reset.S` | 从核进 `secondary_core` 前关当前 EL dcache |
| `smp.c` | 整表 flush/invalidate；`wfe`+周期 `sev`；PSCI 3 次 retry；拉核间 50ms；关 IRQ |
| `smp.c` | 从核 `z_arm64_mm_init` 后立即 `meson_s4_enable_dcache_el1()` |
| `meson_s4_smp_probe.c` | `k_sleep(100ms)` 后再 `z_smp_init()` |

**验证**：`STABILITY_RUNS=5 ./board_test.sh stability` → **5/5 PASS**

---

## 11. 调试历程摘要

| 日期 | 工作 | 结果 |
|------|------|------|
| 2026-07-02 | UART 时钟 retry、怀疑 MMU+dcache | hang 在 soc_prep_hook 后 |
| 2026-07-07 | 测试脚本调通、EL2 dcache disable | 单独使用仍 hang |
| 2026-07-08 | UART TX_EMPTY + 分层 cache | 推进到 MMU/z_cstart |
| 2026-07-08 | PRE_KERNEL_1 UART skip init | **Hello World 成功** |
| 2026-07-08 | UART TX/RX INT_EN + Shell 轮询 | **Shell + SMP 4 核在线** |
| 2026-07-09 | `IRQ_CONNECT` + DTS flags、GIC 诊断 | **混合/纯中断 Shell 均通过** |

---

## 12. 关键约束汇总

- **SECMON** `0x05000000~0x08200000`：须避开
- **dcache**：EL2/EL1/MMU/z_cstart 分层维护，先 MMU 后 dcache
- **UART PRE_KERNEL_1**：不走 k_mem_map_phys_bare，不 poke 寄存器
- **SMP**：见 [smp_psci_boot.md](smp_psci_boot.md)；从核须 `meson_s4_enable_dcache_el1()` 后再清 boot_params
- **串口**：UART_B @ `0xFE07A000`，921600
- **load 地址**：`0x01000000`（= BL33 NS_BL33_ENTRYPOINT）
