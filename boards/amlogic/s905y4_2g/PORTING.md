# Amlogic S4 Zephyr 移植摘要

Board: `s905y4_2g` / SoC: `meson_s4` (S905W2/S905Y4)

## 文档

详细内容已拆分至 [doc/](doc/README.md)：

| 文档 | 内容 |
|------|------|
| [doc/debug.md](doc/debug.md) | 实板调试方法、boot marker、故障树 |
| [doc/bl33_to_zephyr.md](doc/bl33_to_zephyr.md) | U-Boot BL33 → Zephyr 跳转分析 |
| [doc/zephyr_boot_flow.md](doc/zephyr_boot_flow.md) | Zephyr 启动详细流程 |
| [doc/porting_issues.md](doc/porting_issues.md) | 移植问题与解决方案 |

环境与测试：[Test_env.md](Test_env.md)  
镜像打包：[BOOT.md](BOOT.md)

## 当前状态（2026-07-08）

实板验证通过：`Hello World! s905y4_2g/meson_s4`

主要修复：

1. **UART TX_EMPTY 等待** — `meson_s4_early_uart.c` / `uart_meson.c`
2. **分层 cache 维护** — EL2/EL1/MMU/z_cstart 各阶段 flush
3. **PRE_KERNEL_1 UART skip init** — 复用静态 identity map @ `0xFE07A000`

自动化测试：

```bash
./board_test.sh boot
```

## 关键约束

- SECMON `0x05000000~0x08200000` 须避开
- load/entry = `0x01000000`（BL33 NS_BL33_ENTRYPOINT）
- UART_B @ `0xFE07A000`，921600
