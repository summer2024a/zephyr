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

## 8. 调试历程摘要

| 日期 | 工作 | 结果 |
|------|------|------|
| 2026-07-02 | UART 时钟 retry、怀疑 MMU+dcache | hang 在 soc_prep_hook 后 |
| 2026-07-07 | 测试脚本调通、EL2 dcache disable | 单独使用仍 hang |
| 2026-07-08 | UART TX_EMPTY + 分层 cache | 推进到 MMU/z_cstart |
| 2026-07-08 | PRE_KERNEL_1 UART skip init | **Hello World 成功** |

---

## 9. 关键约束汇总

- **SECMON** `0x05000000~0x08200000`：须避开
- **dcache**：EL2/EL1/MMU/z_cstart 分层维护，先 MMU 后 dcache
- **UART PRE_KERNEL_1**：不走 k_mem_map_phys_bare，不 poke 寄存器
- **SMP**：见 [smp_psci_boot.md](smp_psci_boot.md)；从核须 `meson_s4_enable_dcache_el1()` 后再清 boot_params
- **串口**：UART_B @ `0xFE07A000`，921600
- **load 地址**：`0x01000000`（= BL33 NS_BL33_ENTRYPOINT）
