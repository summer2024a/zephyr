# Zephyr 启动详细流程（S4）

从 U-Boot `bootm` 跳转到 `main()` 的完整启动链，含 boot marker 对应关系。

## 1. 总览

```text
bootm → _start (reset.S)
  → EL 检测与切换 (EL3/EL2 → EL1)
  → z_arm64_el*_plat_init()
  → z_prep_c()
      soc_prep_hook → arch_bss_zero → arch_data_copy
      z_arm64_mm_init() → z_arm64_interrupt_init()
      z_cstart()
  → z_sys_init_run_level(EARLY → PRE_KERNEL → POST_KERNEL → ...)
  → main()
```

## 2. 汇编入口（reset.S）

| 步骤 | 函数 | Marker | 说明 |
|------|------|--------|------|
| Reset hook | `meson_s4_reset_hook` | — | S4 平台汇编 hook |
| 最高 EL init | `z_arm64_el_highest_plat_init()` | `H` | 输出 `Zephyr SPL (S4)` |
| EL2 init | `z_arm64_el2_plat_init()` | `G`/`g` | GIC SRE 等（BL31 已配大部分） |
| EL1 init | `z_arm64_el1_plat_init()` | `L` | flush dcache，清 SCTLR.C/M/I，TLBI |
| 进入 C | `z_prep_c()` | — | 不再返回 |

### z_arm64_el1_plat_init 关键操作

```text
arch_dcache_flush_and_invd_all()   # BL31/U-Boot 遗留 dirty lines
清 SCTLR_EL1: M, C, I, ENDIANNESS, ALIGNMENT_FAULT
tlbi vmalle1                       # 清空 TLB
```

此时 MMU 和 dcache 均关闭，为 Zephyr 重建页表做准备。

## 3. C 环境准备（prep_c.c）

| Marker | 函数/操作 | 说明 |
|--------|-----------|------|
| `P` | `soc_prep_hook()` | 平台 prep hook |
| `1` | after soc_prep_hook | |
| `2` | `write_tpidrro_el0()` | CPU 结构体指针 |
| `3` | `arch_bss_zero()` | BSS 清零 |
| `4` | `arch_data_copy()` | .data 从 ROM 复制 |
| `5` | before MMU init | |
| `a` | `z_arm64_mm_init(true)` 返回 | MMU 已开启 |
| `b` | `z_arm64_interrupt_init()` 返回 | |
| — | `z_cstart()` | 进入内核，不再返回 |

## 4. MMU 初始化（mmu.c）

`z_arm64_mm_init()` 流程：

```text
A  进入 z_arm64_mm_init
B  setup_page_tables() 完成
   ├─ 内核 flat ranges（代码/数据/BSS）
   └─ mmu_regions.c 外设 identity map（GIC/APB4/MMC/ETH/USB）
C  enable_mmu_el1 开始
D  页表 flush 完成
E  SCTLR.M 开启（MMU on，dcache 仍 off — S4 专用）
G  dcache 延迟开启标记
```

S4 专用策略：

1. **TCR PTW 设为 Non-Cacheable**：避免页表遍历读 stale cache
2. **先开 MMU，后开 dcache**：dcache 延迟到 `z_cstart()`
3. **`arch_dcache_flush_and_invd_all()`** 在 MMU 使能前执行

外设静态映射（`mmu_regions.c`）：

| 区域 | PA/VA | 大小 |
|------|-------|------|
| GIC-400 | 0xFFF01000 | 多段 |
| APB4_BUS | 0xFE000000 | 0x480000（含 UART 0xFE07A000） |
| MMC/ETH/USB | 各自基址 | identity map |

## 5. 内核入口（init.c z_cstart）

| Marker | 操作 | 说明 |
|--------|------|------|
| — | `meson_s4_enable_dcache_el1()` | flush + 开启 SCTLR.C |
| `C` | cache 就绪 | |
| `d` | `gcov_static_init()` | |
| `e` | `INIT_LEVEL_EARLY` | 早期 init |
| `f` | `arch_kernel_init()` | |
| `g` | `LOG_CORE_INIT()` | |
| `h1/h2` | dummy thread init | 多线程 |
| `i1/i2` | `z_device_state_init()` | 设备状态 |
| `j` | `soc_early_init_hook()` | |
| `k` | `board_early_init_hook()` | |
| `l` | `INIT_LEVEL_PRE_KERNEL_1` | **UART/GIC/PSCI 驱动 init** |
| `m` | `INIT_LEVEL_PRE_KERNEL_2` | |
| … | POST_KERNEL / APPLICATION | `main()` |

## 6. 驱动初始化时序

PRE_KERNEL_1 阶段典型 init 顺序（取决于 Kconfig）：

```text
intc_gic_init()        → GIC 驱动（identity map @ 0xFFF01000）
meson_uart_init()      → UART 驱动（S4: 跳过 register poke）
psci_init()            → PSCI CPU ops（可 defer SMC）
...
uart_console_init()    → 控制台绑定 UART
```

S4 UART 驱动在 PRE_KERNEL_1 **不调用 `DEVICE_MMIO_MAP`**，直接使用静态 identity map 地址 `0xFE07A000`（详见 [porting_issues.md](porting_issues.md)）。

## 7. Early UART vs 正式 UART 驱动

| 阶段 | 组件 | 访问方式 |
|------|------|----------|
| EL 最高 ~ z_cstart | `meson_s4_early_uart.c` | 直接 phys `0xFE07A000` |
| PRE_KERNEL_1+ | `uart_meson.c` | S4: 固定 phys，不走 virt map |
| POST_KERNEL | `printk` / Shell | 经 uart_console → poll_out |

Early UART 在 `s4_uart_early_init()` 中完成 FIFO 复位和 TX/RX 使能；正式驱动不再重复 init。

## 8. Cache 维护时间线

```text
U-Boot bootm          （无 cleanup_before_linux，cache 状态遗留）
  ↓
EL2 z_arm64_el2_init   CONFIG_ARM64_BOOT_DISABLE_DCACHE: flush + 关 dcache
  ↓
EL1 plat init          arch_dcache_flush_and_invd_all + 清 SCTLR.C/M
  ↓
enable_mmu_el1         flush + MMU on（S4: dcache 仍 off）
  ↓
z_cstart               meson_s4_enable_dcache_el1(): flush + dcache on
  ↓
PRE_KERNEL_1+          正常运行（device nGnRnE 映射）
```

## 9. 成功启动完整 marker 示例

```text
Zephyr SPL (S4)
H          ← el_highest_plat_init
G g        ← el2_plat_init
L          ← el1_plat_init（MMU/dcache 已关）
P          ← soc_prep_hook
12345      ← prep_c 各步
ab         ← MMU init + interrupt init
Cdefg      ← z_cstart 子步骤
1212jk     ← multithreading / device state / early hooks
lms4...    ← PRE_KERNEL / POST_KERNEL
Hello World! s905y4_2g/meson_s4
```

## 10. 关键源文件

| 文件 | 阶段 |
|------|------|
| `arch/arm64/core/reset.S` | 汇编入口、EL 切换 |
| `soc/amlogic/s4/meson_s4_boot_debug.S` | reset hook |
| `soc/amlogic/s4/meson_s4_plat.c` | EL2/EL1 plat init |
| `soc/amlogic/s4/meson_s4_early_uart.c` | boot marker UART |
| `arch/arm64/core/prep_c.c` | z_prep_c |
| `arch/arm64/core/mmu.c` | MMU 页表 |
| `soc/amlogic/s4/mmu_regions.c` | 外设映射 |
| `kernel/init.c` | z_cstart |
| `drivers/serial/uart_meson.c` | UART 驱动 |
