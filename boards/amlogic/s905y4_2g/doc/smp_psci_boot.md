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

## 9. 已知问题与排查方向

### 9.1 主核 wfe 永久等待（高度怀疑）

**现象**：`arch_cpu_start()` 中 `while (arm64_cpu_boot_params.fn) wfe()` 不返回。

**原因分析**：S4 主核在 `z_cstart()` 通过 `meson_s4_enable_dcache_el1()` 开启 dcache；从核 `z_arm64_mm_init(false)` 走 S4 的 `enable_mmu_el1()` **延迟 dcache**（与主核相同策略），但 `arch_secondary_cpu_init()` **未**再开 dcache 就写 `fn = NULL`。若从核 store 未与主核 cache 一致，主核可能一直读到 stale `fn`。

**修复（2026-07-08 尝试）**：在 `meson_s4_plat.c` 实现 `soc_per_core_init_hook()`，从核在清 `fn` 前调用 `meson_s4_enable_dcache_el1()`。需 `CONFIG_SOC_PER_CORE_INIT_HOOK`（S4 Kconfig 在 `SMP=y` 时 auto select）。

实板验证：Hello World 仍正常；Shell `meson_s4_smp` 输出待确认（可能 Shell 未就绪或仍 hang）。

### 9.2 MPIDR 与 DT reg 不一致

Zephyr `cpu_node_list[]` = `{0,1,2,3}`（DT 1-cell reg）。U-Boot `get_core_id()` 对 MPIDR bit24 和多 cluster 有特殊处理；S4 单 cluster 4 核通常 AFF0=0..3。

**排查**：Shell 打印 `MPIDR=0x...`（`meson_s4_smp`），对比 `cpu_node_list`。

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
