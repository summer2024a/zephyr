# S4 多核启动与 PSCI 流程

Board: `s905y4_2g` / SoC: `meson_s4` (4×Cortex-A55)

单核启动见 [zephyr_boot_flow.md](zephyr_boot_flow.md)，BL33 跳转见 [bl33_to_zephyr.md](bl33_to_zephyr.md)。

## 1. 总体架构

```text
BL31 (EL3, PSCI 1.0 SMC)
  │
  ├─ CPU0: U-Boot bootm → Zephyr _start → 主核完整 init
  │
  └─ CPU1~3: 上电后处于 OFF / WFE，等待 PSCI CPU_ON
              │
              └─ z_smp_init() → pm_cpu_on(mpid, &__start)
                    └─ 从核从 _start 重入 → arch_secondary_cpu_init()
```

| 组件 | 运行级别 | 多核职责 |
|------|----------|----------|
| BL31 | EL3 | 实现 PSCI SMC（CPU_ON/OFF/SUSPEND） |
| U-Boot | EL1 | 仅启动 CPU0，不拉从核 |
| Zephyr CPU0 | EL1 | MMU/GIC/Shell，`z_smp_init()` 拉从核 |
| Zephyr CPU1~3 | EL1 | `reset.S` → `arch_secondary_cpu_init()` → 调度器 |

## 2. 设备树与 PSCI 配置

`boards/amlogic/common/meson_s4_common.dtsi`：

```dts
cpus {
    cpu@0 { reg = <0x0>; enable-method = "psci"; };
    cpu@1 { reg = <0x1>; enable-method = "psci"; };
    cpu@2 { reg = <0x2>; enable-method = "psci"; };
    cpu@3 { reg = <0x3>; enable-method = "psci"; };
};

psci {
    compatible = "arm,psci-1.0";
    method = "smc";          /* 走 SMC 进 EL3，非 HVC */
};
```

Kconfig（`s905y4_2g_defconfig`）：

| 选项 | 作用 |
|------|------|
| `CONFIG_SMP=y` | 启用 SMP 调度 |
| `CONFIG_SMP_BOOT_DELAY=y` | **延迟**从核启动，仅 CPU0 跑到 Shell |
| `CONFIG_MP_MAX_NUM_CPUS=4` | 4 核 |
| `CONFIG_PM_CPU_OPS_PSCI=y` | PSCI 驱动 |
| `CONFIG_GIC_V2=y` | GIC-400 + SGI IPI |

## 3. PSCI CPU_ON 调用链

### 3.1 SMC 入口

```text
z_smp_init()                          kernel/smp.c
  └─ start_cpu(id, ...)                 kernel/smp.c
       └─ arch_cpu_start(...)           arch/arm64/core/smp.c
            ├─ 填写 arm64_cpu_boot_params { mpid, sp, fn, arg, cpu_num }
            ├─ sys_cache_data_flush_range(&arm64_cpu_boot_params)
            └─ pm_cpu_on(cpu_mpid, &__start)    drivers/pm_cpu_ops/pm_cpu_ops_psci.c
                 └─ arm_smccc_smc(PSCI_CPU_ON, cpuid, entry_point, 0)
                      └─ BL31 PSCI handler @ EL3
```

`pm_cpu_on` 实现（`pm_cpu_ops_psci.c`）：

```c
ret = psci_data.invoke_psci_fn(PSCI_FN_NATIVE(0_2, CPU_ON),
                               cpuid, (unsigned long)entry_point, 0);
```

参数：

| 参数 | S4 值 | 说明 |
|------|-------|------|
| `cpuid` | `cpu_node_list[i]` = 0,1,2,3 | DT `reg`，与 U-Boot `psci_cpu_on(cpu, ep)` 一致 |
| `entry_point` | `&__start` | 从核入口，重走 `reset.S` |
| 返回值 | 0 = 成功 | `-EPERM` / `-EINVAL` 等见 PSCI 规范 |

PSCI 在 PRE_KERNEL_1 由 `psci_init()` 探测版本（`PSCI_VERSION` SMC）。

### 3.2 U-Boot 对比

Amlogic U-Boot `arch/arm/mach-meson/smp.c`：

```c
int cpu_up(unsigned int cpu, unsigned int entrypoint)
{
    return psci_cpu_on(cpu, entrypoint);  /* cpu = 0..3，非完整 MPIDR */
}
```

从核入口 `secondary_start()` 会先 `enable_caches()`，再执行 boot 回调。Zephyr 从核走 `reset.S` 完整路径，在 `z_arm64_el1_plat_init()` 中 flush 并关 MMU/dcache 后重建。

### 3.3 与 Linux 启动对比（S4 / meson-s4.dtsi）

**原则**：SMP 问题先对照 Linux 已验证路径，再改 Zephyr；不盲目堆 SMP patch。

| 维度 | Linux (`linux-meson64-6.12`) | Zephyr S4 |
|------|------------------------------|-----------|
| 主核入口 | U-Boot → `primary_entry` → `__primary_switch` | U-Boot → `__start` → `z_prep_c` → `z_cstart` |
| 从核入口 | PSCI → **`secondary_entry`**（专用，不重跑主核 init） | PSCI → **`__start`**（与冷启动相同，重走 EL 降级 + plat init） |
| PSCI CPU_ON 参数 | `cpu_logical_map(cpu)`：boot CPU 用硬件 MPIDR，其余来自 DT `reg` | `cpu_node_list[i]`：DT 1-cell `reg` = 0..3（与 U-Boot Meson 一致） |
| DT CPU `reg` | `#address-cells=2`：`<0x0 0x0>` … `<0x0 0x3>`（AFF0） | `#address-cells=1`：`<0x0>` … `<0x3>` |
| 从核 MMU/cache | `secondary_startup` → `__cpu_setup` → `__enable_mmu`；**MMU+dcache+icache 一次打开**（`INIT_SCTLR_EL1_MMU_ON`） | `z_arm64_el1_init` → **`z_arm64_el1_plat_init` 关 MMU/dcache** → `z_arm64_mm_init(false)` → S4 **延迟 dcache** |
| 主从同步 | `secondary_data.task` + `update_cpu_boot_status()` + `complete(&cpu_running)`；失败时 MMU off 下 `dc ivac` | `arm64_cpu_boot_params.fn`：从核清 NULL + `sev()`，主核 `while(fn) wfe()` |
| 主核拉核时机 | `kernel/smp.c` `__cpu_up` → `cpu_psci_cpu_boot` | `z_smp_init` → `arch_cpu_start`（可 `SMP_BOOT_DELAY` 延迟） |

Linux 从核路径（`arch/arm64/kernel/head.S`）：

```text
secondary_entry
  └─ init_kernel_el              # 规范化 EL，SCTLR_MMU_OFF
  └─ secondary_startup
       ├─ __cpu_setup
       ├─ __enable_mmu            # INIT_SCTLR_EL1_MMU_ON：M+C+I 同时开
       └─ __secondary_switched
            ├─ secondary_data.task 非空检查
            └─ secondary_start_kernel()
                 └─ update_cpu_boot_status(SUCCESS); complete()
```

Linux PSCI（`arch/arm64/kernel/psci.c`）：

```c
phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
```

**关键差异**：

1. **入口**：Linux 从核不经过 `primary_entry` 的 MMU 探测/清理；Zephyr 从核与 CPU0 共用 `__start`，必经 `z_arm64_el1_plat_init()` 的 flush+关 cache/MMU。
2. **Cache 策略**：Linux 从核 MMU 使能时 **同步开 dcache**；Zephyr S4 主从均延迟 dcache，主核在 `z_cstart()` 开，从核依赖 `soc_per_core_init_hook()`（见 §3.4）。
3. **同步原语**：Linux 用 `dsb(ishst)` + completion；Zephyr 用共享结构体 + `wfe`/`sev`，对 **主核 dcache 已开、从核可能未开** 更敏感。

### 3.4 SMP 路径 Cache 分析

单核 boot 的 cache 分层见 [porting_issues.md](porting_issues.md) §2。SMP 在此基础上增加 **主从不对称**：

```text
                    CPU0 (主核)                         CPU1~3 (从核)
                    ─────────                         ─────────────
PSCI 之前           z_cstart: dcache ON               OFF / 刚被 PSCI 唤醒
boot_params 写入    flush → pm_cpu_on                 读 mpid/sp（dcache 可能 OFF）
EL1 plat init       冷启动已执行                       PSCI 后再次执行 z_arm64_el1_plat_init
MMU                 z_arm64_mm_init(true)             z_arm64_mm_init(false)，复用 kernel_ptables
dcache 开启点       z_cstart → meson_s4_enable_*      enable_mmu 延迟 → soc_per_core_init_hook
fn=NULL 同步        while(fn) wfe() [dcache ON]       barrier + fn=NULL + sev [需 dcache ON]
```

| 阶段 | 主核 cache 状态 | 从核 cache 状态 | 风险 |
|------|----------------|----------------|------|
| `arch_cpu_start` 写 boot_params | dcache **ON** | — | 已 `sys_cache_data_flush_range` ✓ |
| 从核 `reset.S` 读 mpid | dcache ON | **OFF**（EL1 plat 后） | 读 RAM 通常 OK |
| 从核 `z_arm64_mm_init` | dcache ON | MMU on，dcache **OFF**（S4） | 页表 walk 用 NC PTW，与单核一致 |
| 从核写 `fn = NULL` | 轮询 dcache | **若未开 dcache**：store 直达 RAM | 主核可能读 stale cache line |
| 主核 `wfe` 循环 | 读 `fn` 走 dcache | — | **9.1 所述 hang 根因** |

Linux 侧对照（`head.S` / `smp.h`）：

- 主核 `primary_entry`：按 **进内核时 MMU 是否已开** 选择 `dcache_inval_poc` 或 `dcache_clean_poc`（与 U-Boot 遗留 dirty line 处理思路一致）。
- 从核 `__enable_mmu`：`INIT_SCTLR_EL1_MMU_ON` 含 `SCTLR_ELx_C`，**不会出现「主开从不开」就写共享变量**。
- `update_cpu_boot_status()`：`WRITE_ONCE` + `dsb(ishst)`；MMU off 失败路径用 `dc ivac` 写 `__early_cpu_boot_status`。

**S4 standalone boot 注意**：U-Boot `bootm` **不调用** `cleanup_before_linux()`（单核 doc 已述），PSCI 热启动从核同样继承 BL31/U-Boot 遗留状态，但 Zephyr 会再次 `arch_dcache_flush_and_invd_all()`（`z_arm64_el1_plat_init`）。

**已做缓解（待实板确认 hang 点）**：

- `soc_per_core_init_hook()`：从核清 `fn` 前 `meson_s4_enable_dcache_el1()`。
- 主核 `arch_cpu_start`：`sys_cache_data_flush_range(&arm64_cpu_boot_params)`。

**下一步（先定位，再改代码）**：

1. Shell `meson_s4_smp` 记录 `MPIDR=0x...`（见 §9.2 MPIDR 编码差异）。
2. 开 `CONFIG_SOC_MESON_S4_BOOT_TRACE`，在 `reset.S` secondary 分支 / `arch_secondary_cpu_init` 各阶段打 marker，确认 hang 在 **secondary_core 等 mpid**、**MMU init** 还是 **fn 同步**。
3. 若确认只在 fn 同步：评估 `sys_cache_data_flush_range` 清 fn 或统一 MPIDR 编码；**不**在未定位前继续改 SMP 逻辑。

## 4. 从核汇编入口（reset.S）

PSCI CPU_ON 使从核从 `__start` 执行，与 CPU0 冷启动相同入口，但 `arm64_cpu_boot_params.mpid` 已被主核预设。

```text
__start
  ├─ [SMP] voting lock 选主核（PSCI 热启动时 mpid≠-1，从核直接进 secondary_core）
  │
  ├─ primary_core → z_prep_c → z_cstart → ...     (仅 CPU0)
  │
  └─ secondary_core:
       loop: mpidr == arm64_cpu_boot_params.mpid ?   (等待主核指定本核)
       load sp, fn from boot_params
       → z_arm64_secondary_prep_c
            → arch_secondary_cpu_init()
```

关键数据结构 `arm64_cpu_boot_params`（`arch/arm64/core/smp.c`）：

| 字段 | 主核写入 | 从核读取 |
|------|----------|----------|
| `mpid` | 目标核 MPIDR | `reset.S` 等待匹配 |
| `sp` | 从核 ISR 栈顶 | 设 SP |
| `fn` | `smp_init_top` | 执行后 **置 NULL** 通知主核 |
| `cpu_num` | 逻辑 CPU id | `tpidrro_el0` / GIC |

主核同步：

```c
while (arm64_cpu_boot_params.fn) {
    wfe();    /* 等从核 arch_secondary_cpu_init 清除 fn 并 sev() */
}
```

## 5. 从核 C 入口（arch_secondary_cpu_init）

```text
arch_secondary_cpu_init()             arch/arm64/core/smp.c
  ├─ write_tpidrro_el0(&_kernel.cpus[cpu_num])
  ├─ z_arm64_mm_init(false)           不再建页表，enable_mmu_el1 复用 kernel_ptables
  ├─ arm_gic_secondary_init()         GIC CPU interface
  ├─ irq_enable(SGI_SCHED_IPI)        调度 IPI
  ├─ arm64_cpu_boot_params.fn = NULL  释放主核 wfe 循环
  ├─ sev()
  └─ fn(arg) → smp_init_top()         kernel/smp.c
       ├─ atomic_set(&ready_flag, 1)  通知 start_cpu()
       ├─ wait cpu_start_flag
       ├─ z_dummy_thread_init()
       └─ z_swap_unlocked()            进入调度器
```

## 6. 主核 z_smp_init 时序

```text
bg_thread_main()                      kernel/init.c
  └─ [无 BOOT_DELAY] z_smp_init()     POST_KERNEL 末尾
  └─ [BOOT_DELAY]  跳过，等 meson_s4_smp shell 命令

z_smp_init()                          kernel/smp.c
  ├─ atomic_clear(&cpu_start_flag)    从核需等待此 flag
  ├─ for (i = 1; i < num_cpus; i++)
  │     z_init_cpu(i)
  │     start_cpu(i, NULL)
  │       └─ arch_cpu_start → pm_cpu_on → 等 ready_flag
  └─ atomic_set(&cpu_start_flag, 1)    允许从核继续 init
```

`arch_smp_init()`（PRE_KERNEL_1）仅注册 SGI IPI handler，**不**拉从核。

## 7. GIC 与 SMP IPI

| SGI | 用途 | 注册位置 |
|-----|------|----------|
| 0 | `SGI_SCHED_IPI` 调度 IPI | `arch_smp_init()` |
| 1 | `SGI_MMCFG_IPI` userspace | 可选 |
| 2 | `SGI_FPU_IPI` FPU flush | 可选 |

从核 `arm_gic_secondary_init()` 使能 GIC CPU interface；UART SPI 169 由 GIC distributor 统一管理。

## 8. 当前 S4 配置与实板状态

### 8.1 CONFIG_SMP_BOOT_DELAY

| 模式 | 行为 | 实板结果 |
|------|------|----------|
| `SMP_BOOT_DELAY=n` | POST_KERNEL 自动 `z_smp_init()` | **Hang**（Hello World 不出） |
| `SMP_BOOT_DELAY=y` | 仅 CPU0 启动 | **Hello World OK** |
| Shell `meson_s4_smp` | 手动 `z_smp_init()` | **Hang**（2026-07-08） |

单核到 Shell/Hello World 在 `BOOT_DELAY` 模式下是 **预期行为**，不代表 4 核已在线。

### 8.2 验收命令

（需 Shell 就绪；自动化若未见 `uart:~`，用 §8.4 `SMP_AUTO_PROBE`。）

```text
uart:~$ meson_s4_smp
uart:~$ kernel threads
```

期望：

```text
Secondary CPU core 1 (MPID:0x1) is up
Secondary CPU core 2 (MPID:0x2) is up
Secondary CPU core 3 (MPID:0x3) is up
OK: 4 CPUs online (PSCI CPU_ON)
```

### 8.3 实板 SMP trace 结果（2026-07-08）

使用 `CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE=y`（Hello World 后直接 `z_smp_init()`）+ 分层 `printf` trace。

**调用链 trace（已逐段验证）**：

```text
z_smp_init → z_init_cpu(1..3) ✓ → start_cpu ✓ → arch_cpu_start ✓
  → pm_cpu_on(0x1..0x3, &__start) → PSCI SMC
  → [成功] ready_flag / fn 同步 → z_smp_init done
```

| 阶段 | 典型输出 | 结论 |
|------|----------|------|
| `z_init_cpu(1..3)` | `enter` / `after init_idle_thread` / `done` | 正常 |
| `start_cpu` | `before arch_cpu_start` | 正常 |
| `arch_cpu_start` | `curr_cpu=0` `primary=0` `loop_i=N` | 主核 ID 正确；`static i` 跨多次调用递增 |
| PSCI | `pm_cpu_on before smc` → `smc ret=0` | **Hang 点 A**：有时卡在 SMC 内，无 `ret` |
| 同步 | `start_cpu after arch_cpu_start` | **Hang 点 B**：SMC 返回 0 后卡在 `wfe` 等 `fn=NULL` |
| 成功 | `z_smp_init returned, arch_num_cpus()=4` | **cache flush/invalidate 补丁后多次成功** |

**Hang B 缓解（2026-07-08，`arch/arm64/core/smp.c`）**：

```c
/* 从核清 fn 后 flush 到 PoC */
sys_cache_data_flush_range(&arm64_cpu_boot_params.fn, sizeof(...));

/* 主核 wfe 轮询前 invalidate，避免 stale cache line */
sys_cache_data_invd_range(&arm64_cpu_boot_params.fn, sizeof(...));
```

配合既有 `soc_per_core_init_hook()` → `meson_s4_enable_dcache_el1()`（从核写 `fn` 前开 dcache）。

**Hang B 进一步加固（2026-07-09）**：

- `reset.S`：从核 `secondary_core` 前关当前 EL dcache（避免 BL31 遗留 cache 读 stale `mpid`）
- `smp.c`：整表 flush/invalidate；`wfe`+周期 `sev`；PSCI 3 次 retry；拉核间 50ms；`arch_cpu_start` 关 IRQ
- 从核 `z_arm64_mm_init` 后立即开 dcache（不再等到 GIC 后）
- `meson_s4_smp_probe.c`：`k_sleep(100ms)` 后再 `z_smp_init()`

实板 `STABILITY_RUNS=5 ./board_test.sh stability` → **5/5 PASS**（irq-full + SMP auto-probe）。

**MPIDR（CPU0）**：`0x80000000`，`MPIDR_TO_CORE=0x0`，与 DT `reg=0` 一致。

**Boot trace 与 Shell**：全量 `BOOT_TRACE` 时自动化测试仍难见 `uart:~`；SMP trace 用 `SMP_AUTO_PROBE` + `printf` 即可。

### 8.4 SMP trace marker 表（`CONFIG_SOC_MESON_S4_BOOT_TRACE`）

| Marker | 位置 | 含义 |
|--------|------|------|
| `Q` | `arch_cpu_start` 入口 | 主核开始拉从核 |
| `+` | `pm_cpu_on` 前 | boot_params 已 flush |
| `=` | `pm_cpu_on` 返回后 | SMC 成功 |
| `9` | `wfe` 循环退出 | 从核已清 `fn` |
| `<` | `z_arm64_secondary_prep_c` | 从核 C 入口 |
| `>` | `arch_secondary_cpu_init` | 从核 init 开始 |
| `{` | `z_arm64_mm_init(false)` 后 | 从核 MMU |
| `}` | `arm_gic_secondary_init` 后 | 从核 GIC |
| `~` | `soc_per_core_init_hook` 后 | 从核 dcache 已开 |
| `!` | `fn=NULL; sev()` 后 | 主从同步完成 |

调试 Kconfig：

| 选项 | 用途 |
|------|------|
| `CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE` | Hello 后自动 `z_smp_init()`（仅 debug） |
| `EXTRA_CMAKE_ARGS=-DCONFIG_SOC_MESON_S4_BOOT_TRACE=y` | 全量 early marker（与 Shell 同开可能干扰） |

## 9. 已知问题与排查方向

> **更新（2026-07-08 晚）**：默认 defconfig + Shell `meson_s4_smp` 路径已在实板验收 **4 核在线**（`arch_num_cpus()=4`）。根因之一是 Meson UART 中断未正确门控导致 IRQ 风暴（见 [porting_issues.md](porting_issues.md) §10）。PSCI `CPU_ON` 仍偶发 Hang A（SMC 内），需多次复位统计。

### 9.1 Hang 点（实板 trace 确认，2026-07-08）

**路径已逐段打通**：`z_init_cpu` / `start_cpu` / `arch_cpu_start` 均正常；问题在 PSCI 之后。

| 模式 | 现象 | 怀疑 |
|------|------|------|
| **A — SMC 内 hang** | 有 `before smc`，无 `smc ret=0` | BL31 `CPU_ON` 未返回；需查 TF-A / 从核是否进 `__start` |
| **B — wfe 同步 hang** | `smc ret=0`，无 `start_cpu after` | 主核 `while(fn) wfe()`，从核未清 `fn`（§3.4 cache 不一致） |
| **成功** | 三次 `pm_cpu_on` 均 `ret=0`，`arch_num_cpus()=4` | 从核及时 `arch_secondary_cpu_init` → `fn=NULL` |

**下一步（不盲目改代码）**：

1. ~~Hang B：验证 `soc_per_core_init_hook` + flush/invalidate~~ → **已加**（2026-07-08）：
   - 从核 `fn=NULL` 后 `sys_cache_data_flush_range`
   - 主核 `wfe` 前 `sys_cache_data_invd_range`
   - 实板多次出现 `arch_num_cpus()=4`；多核并发 `printf` 会导致串口乱码
2. Hang A：从核 marker `<` 已在 `z_arm64_secondary_prep_c`（S4+SMP）；乱码中可见 `secondary fn cleared`
3. ~~稳定后改回 Shell `meson_s4_smp` 验收~~ → **已完成**（2026-07-08）：`CONFIG_SHELL_BACKEND_SERIAL_INTERRUPT_DRIVEN=n` + UART IRQ fix 后，`board_test.sh boot` 自动执行 `meson_s4_smp` 报告 **OK: 4 CPUs online**
4. 可选：`CONFIG_SOC_MESON_S4_SMP_DEFER_INIT=y` 用 k_thread 延迟 3s 调 `z_smp_init()`（无需 Shell）

### 9.2 主核 wfe 永久等待（若 arch_cpu_start 能跑到 pm_cpu_on）

**现象**：`arch_cpu_start()` 中 `while (arm64_cpu_boot_params.fn) wfe()` 不返回；无 `Failed to boot secondary CPU` 打印。

**可能原因 A — cache 不一致**（见 §3.4）：主核 dcache ON 轮询 `fn`，从核若在 dcache OFF 时写 `fn = NULL`，主核可能读 stale 值。Linux 从核在 `__enable_mmu` 时即开 dcache，无此不对称。

**可能原因 B — 从核未跑到 `arch_secondary_cpu_init`**：卡在 `reset.S` `secondary_core`（mpid 不匹配）、`z_arm64_el1_init` / `z_arm64_mm_init`，或 PSCI 未真正执行到 `__start`。

**已做缓解（未验证）**：`soc_per_core_init_hook()` → `meson_s4_enable_dcache_el1()`。当前实板 hang 早于 `arch_cpu_start`（§9.1），此缓解尚未触达。

### 9.3 MPIDR 编码（实板已测）

Zephyr 内存在 **三种 MPIDR 用法**，Linux 统一用 `cpu_logical_map`（硬件 MPIDR & `MPIDR_HWID_BITMASK`）：

| 位置 | 算法 | 典型 CPU1 值 |
|------|------|-------------|
| `reset.S` `get_cpu_id` | `mpidr_el1` 低 24 位 | `0x1` |
| `cpu_node_list[]` / `pm_cpu_on` | DT `reg` | `0x1` |
| `smp.c` `MPIDR_TO_CORE()` | `mpidr & (AFF0..2 \| AFF3)` | 若 MPIDR=`0x81000001` → **`0x8100000001`** |

`arm64_cpu_boot_params.mpid` 存的是 DT 值（`0x1`），`reset.S` 比较与 PSCI 参数一致。但 `arch_secondary_cpu_init()` 有：

```c
__ASSERT(arm64_cpu_boot_params.mpid == MPIDR_TO_CORE(GET_MPIDR()), "");
```

若硬件 MPIDR 含 AFF3（Cortex-A55 常见 `0x8100000x`），ASSERT 可能失败。**本板实测 CPU0：`MPIDR=0x80000000`，`MPIDR_TO_CORE=0x0`**，与 DT `reg=0` 一致。

U-Boot `get_core_id()` 对 bit24（MT）和多 cluster 有额外处理；S4 单 cluster 通常 AFF0=0..3 即可。

### 9.3 voting lock .bss 垃圾

`reset.S` 投票锁依赖 `arm64_cpu_boot_params.voting[]` 在 .bss 清零后正确。PSCI 热启动从核重入 `_start` 时若 voting 未清零可能异常。

### 9.4 PSCI SMC 失败

若 `pm_cpu_on` 返回非 0，会打印 `Failed to boot secondary CPU core N`。当前 hang 无此打印，说明 SMC 可能成功、卡在同步。

### 9.5 与 U-Boot 差异

U-Boot 从核 `secondary_start()` 第一件事 `enable_caches()`；Zephyr 从核关 cache/MMU 后重建。S4 需保证 **主从 cache 一致性** 与分层策略一致（见 [porting_issues.md](porting_issues.md) § Cache）。

## 10. 调试方法

### 启用 boot trace

```kconfig
CONFIG_SOC_MESON_S4_BOOT_TRACE=y
# CONFIG_SMP_BOOT_DELAY=y   # 可先关掉观察 POST_KERNEL hang
```

SMP 路径可临时在 `arch_cpu_start` / `arch_secondary_cpu_init` 加 `meson_s4_boot_marker()`。

### 自动化

`board_test.sh boot` 在 Hello World 后自动发 `meson_s4_smp`、`kernel threads`（见 `remote_serial_test.py`）。

### 日志关键字

```bash
grep -E 'Secondary CPU|Failed to boot|PSCI|meson_s4_smp|arch_num_cpus' /tmp/s4_boot_*.log
```

## 11. 关键源文件

| 文件 | 作用 |
|------|------|
| `arch/arm64/core/reset.S` | 投票锁、primary/secondary 分流 |
| `arch/arm64/core/smp.c` | `arch_cpu_start`、`arch_secondary_cpu_init`、`arch_smp_init` |
| `kernel/smp.c` | `z_smp_init`、`start_cpu`、`smp_init_top` |
| `drivers/pm_cpu_ops/pm_cpu_ops_psci.c` | `pm_cpu_on` SMC |
| `drivers/interrupt_controller/intc_gic.c` | GIC + `arm_gic_secondary_init` |
| `soc/amlogic/s4/meson_s4_smp_shell.c` | `meson_s4_smp` shell 命令 |
| `soc/amlogic/s4/meson_s4_plat.c` | `meson_s4_enable_dcache_el1` |
| `boards/amlogic/common/meson_s4_common.dtsi` | CPU / PSCI 节点 |

## 12. 流程图（PSCI 拉核）

```mermaid
sequenceDiagram
    participant CPU0 as CPU0 (主核)
    participant PSCI as BL31 PSCI
    participant CPU1 as CPU1 (从核)
    participant GIC as GIC-400

    CPU0->>CPU0: z_smp_init()
    CPU0->>CPU0: arm64_cpu_boot_params.mpid=1
    CPU0->>CPU0: flush boot_params
    CPU0->>PSCI: SMC CPU_ON(1, __start)
    PSCI->>CPU1: 上电，PC=__start
    CPU1->>CPU1: reset.S secondary_core 等 mpid
    CPU1->>CPU1: arch_secondary_cpu_init()
    CPU1->>CPU1: z_arm64_mm_init(false)
    CPU1->>GIC: arm_gic_secondary_init()
    CPU1->>CPU0: fn=NULL; sev()
    CPU0->>CPU0: 退出 wfe 循环
    CPU1->>CPU1: smp_init_top() → 调度器
    CPU0->>CPU0: ready_flag=1，继续 CPU2/3
```
