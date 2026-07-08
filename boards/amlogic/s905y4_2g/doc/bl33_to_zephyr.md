# BL33 跳转到 Zephyr 分析

本文分析 Amlogic S4 平台上 U-Boot（BL33）经 `bootm` 命令将控制权交给 Zephyr 的完整路径。

## 1. 固件启动链

```text
BL1 (ROM)
  → BL2 (Bootloader)
    → BL31 (ARM Trusted Firmware-A, EL3)
      → BL33 (U-Boot, NS world)
        → bootm zephyr.uimg
          → Zephyr _start @ 0x01000000 (EL1)
```

| 组件 | 运行级别 | 职责 |
|------|----------|------|
| BL31 | EL3 | PSCI、SMC、GIC 基础配置 |
| U-Boot | EL2→EL1 | 外设初始化、网络/存储、加载镜像 |
| Zephyr | EL1 | RTOS 内核与应用 |

BL31 定义 NS world BL33 入口地址 **NS_BL33_ENTRYPOINT = `0x01000000`**。Zephyr 链接脚本与 mkimage 均遵循此地址。

## 2. uImage 格式

```bash
mkimage -A arm64 -O u-boot -T standalone -C none \
    -a 0x01000000 -e 0x01000000 \
    -n "Zephyr S4 S905Y4" \
    -d zephyr.bin zephyr.uimg
```

| 参数 | 值 | 含义 |
|------|-----|------|
| `-T standalone` | 独立程序 | 不经过 Linux 启动协议 |
| `-a 0x01000000` | Load Address | 镜像加载/运行地址 |
| `-e 0x01000000` | Entry Point | 入口 = 加载地址，无需二次搬运 |
| `-C none` | 无压缩 | bootm 直接跳转 |

验证：

```bash
mkimage -l zephyr.uimg
# Load Address: 01000000
# Entry Point:  01000000
```

## 3. U-Boot `bootm` 处理流程

以 TFTP 加载为例：

```text
tftpboot 0x01000000 zephyr.uimg   # 镜像已在目标地址
bootm 0x01000000
```

`bootm` 内部状态机（简化）：

```text
BOOTM_STATE_START
  → BOOTM_STATE_FINDOS      解析 uImage 头，识别 IH_TYPE_STANDALONE
  → BOOTM_STATE_LOADOS      检查 load addr；-a == 当前地址则跳过复制
  → BOOTM_STATE_OS_PREP     standalone 无 Linux prep
  → BOOTM_STATE_OS_GO       调用 do_bootm_standalone()
```

### standalone 入口（bootm_os.c）

```c
appl = (int (*)(int, char *const[]))images->ep;
appl(argc, argv);
```

要点：

1. **无 `cleanup_before_linux()`**：standalone 路径不调用 Linux 专用的 cache/MMU 清理（与 `bootm linux` 不同）
2. **直接函数指针跳转**：`images->ep` = uImage 头中的 `-e` 地址 = Zephyr `_start`
3. **参数可忽略**：Zephyr 入口不依赖 `argc/argv`
4. **autostart**：若 `env get autostart` 非 yes，`do_bootm_standalone` 只设 `filesize` 不执行；s4_ap201 通过 `uenvcmd` 直接 `run loadkernel; bootm` 绕过此限制

### 与 Linux boot 的区别

| 项目 | Linux (`bootm linux`) | Zephyr (`standalone`) |
|------|------------------------|----------------------|
| cache 清理 | `cleanup_before_linux()`：关 icache/dcache、invalidate | **无** |
| MMU | U-Boot 关 MMU 后跳转 | U-Boot 状态原样交给 Zephyr |
| 入口协议 | ATAGs/DTB + 寄存器约定 | 函数指针直接调用 |
| 镜像类型 | IH_TYPE_KERNEL | IH_TYPE_STANDALONE |

**影响**：Zephyr 继承 U-Boot/BL31 遗留的 cache 状态（可能含 dirty dcache lines），须在自身启动各阶段分层维护 cache（见 [porting_issues.md](porting_issues.md)）。

## 4. U-Boot 跳转时 CPU 状态

Zephyr 入口 `_start`（`arch/arm64/core/reset.S`）执行时，典型状态：

| 寄存器/状态 | 值 |
|-------------|-----|
| Exception Level | EL1（U-Boot 已从 EL2 降到 EL1） |
| MMU (SCTLR.M) | 可能 ON 或 OFF（取决于 U-Boot 配置） |
| dcache (SCTLR.C) | 可能 ON，含 BL31/U-Boot dirty lines |
| icache (SCTLR.I) | 通常 ON |
| 入口 PC | `0x01000000`（Zephyr `_start`） |
| 串口 | U-Boot 已初始化 UART_B，Zephyr early UART 可复用 |

Zephyr 在 `z_arm64_el1_plat_init()` 中主动 flush dcache 并清除 SCTLR.C/M/I，重建 MMU，不依赖 U-Boot 的 `cleanup_before_linux()`。

## 5. 内存布局约束

```text
0x01000000 ~ 0x7FFFFFFF   DDR（Zephyr 代码/数据/BSS）
0x05000000 ~ 0x08200000   SECMON（BL31 Secure-only，DTS/MMU 须避开）
0xFE000000 ~ 0xFE47FFFF   APB4 外设（UART @ 0xFE07A000）
0xFFF01000 ~              GIC-400
```

Zephyr 链接：

- `ROM_ADDR = RAM_ADDR = 0x01000000`
- 代码从 `__text_region_start` 起，与 uImage load/entry 一致

## 6. TFTP 自动化路径（board_test.sh）

```text
142 USB 复位
  → U-Boot autoboot（bootdelay=1）
  → uenvcmd: tftpboot 0x01000000 zephyr.uimg
  → bootm 0x01000000
  → Zephyr _start
85  串口捕获全程日志
```

U-Boot 环境变量（s4_ap201）：

| 变量 | 值 |
|------|-----|
| `serverip` | `192.168.53.142` |
| `ipaddr` | `192.168.53.130` |
| `loadkernel` | `tftpboot 0x01000000 zephyr.uimg` |
| `uenvcmd` | `run loadkernel; bootm 0x01000000` |

## 7. 与 baremetal_test 对比

U-Boot 侧 `bl33/v2023/baremetal_test/` 使用相同 mkimage 参数和 `bootm` 命令，已验证 standalone 路径可用。Zephyr 与之差异仅在镜像内容（完整 RTOS vs 汇编裸机），跳转机制完全相同。

| 项目 | baremetal_test | Zephyr |
|------|---------------|--------|
| 链接地址 | 0x01000000 | 0x01000000 |
| mkimage -T | standalone | standalone |
| bootm 命令 | `bootm 0x01000000` | 相同 |
| 入口处理 | 直接 `_start` | 直接 `_start` → reset.S → z_prep_c |

## 8. 多核（PSCI）

BL31 提供 PSCI 1.0 SMC。Zephyr 主核从 `0x01000000` 启动后，通过 `PSCI CPU_ON` 唤醒 CPU1~3（需 `CONFIG_SMP=y`）。PSCI 调用在 PRE_KERNEL 阶段进行，可 defer 以避免早期 hang。

参考：`soc/amlogic/s4/meson_s4_smp_shell.c` 中 `meson_s4_smp` shell 命令查看在线核数。
