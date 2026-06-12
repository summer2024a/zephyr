# Amlogic S4 SoC Support

This directory contains Zephyr SoC support for the Amlogic S4
(S905W2/S905Y4) family of ARM64 SoCs.

## Key Hardware Features

- 4x Cortex-A55 CPU cores
- PSCI 1.0 (SMC) for secondary CPU boot
- GIC-400 interrupt controller
- Meson UART (not ns16550 compatible)
- DesignWare MAC 3.70a (RMII, internal PHY)
- Amlogic Meson AXG MMC (SD + eMMC)
- XHCI USB host (generic-xhci @ 0xFDE00000)

## Boot Flow

```
BL1 (ROM) → BL2 (Bootloader) → BL31 (ARM TF-A, EL3)
    → BL33 (Zephyr) enters at 0x01000000 (NS_BL33_ENTRYPOINT)
    → Secondary CPUs brought up via PSCI CPU_ON SMC calls
```

BL31 runs at EL3 and uses PSCI for CPU_ON. Zephyr runs at EL1
after EL2→EL1 transition in `meson_s4_plat.c`.

## Source Files

| File | Purpose |
|------|---------|
| `meson_s4_plat.c` | EL2→EL1 transition, SCTLR fixup, boot markers |
| `meson_s4_early_uart.c` | Early UART_B output (register-level, before driver init) |
| `meson_s4_boot_debug.S` | Assembly boot marker stub (called from reset.S) |
| `meson_s4_smp_shell.c` | Shell command `meson_s4_smp` for SMP status check |
| `mmu_regions.c` | MMU memory mapping: GIC, APB4, MMC, ETH, USB, XHCI |

## MMU Regions

```
Region              Physical            Size        Attributes
GIC-400             0xFFF01000          16 KB+64 KB Device
APB4 bus            0xFE000000          4.5 MB      Device
MMC (SD)            0xFE08A000          2 KB        Device
MMC (eMMC)          0xFE08C000          2 KB        Device
ETH (DW MAC)        0xFDC00000          64 KB       Device
USB XHCI            0xFDE00000          1 MB        Device
USB2 PHY0           0xFE003C000         8 KB        Device
USB2 PHY1           0xFE003E000         8 KB        Device
```

## Zephyr Configuration

Key Kconfig options:
- `CONFIG_SOC_AMLOGIC_MESON_S4`: Select S4 SoC
- `CONFIG_SOC_MESON_S4_BOOT_TRACE`: Enable early UART boot markers
- `CONFIG_SOC_MESON_S4_SHELL_SMP`: SMP status shell command
- `CONFIG_ARM_PSCI`: Use PSCI for secondary CPU boot

## Driver Status

| Driver | File | Status |
|--------|------|--------|
| UART (console) | `drivers/serial/uart_meson.c` | ✅ Working |
| PSCI SMP | Zephyr `pm_cpu_ops_psci` | ✅ Working (4 cores) |
| MMC/SDHC | `drivers/sdhc/sdhc_meson_axg_mmc.c` | ⚠️ Development |
| Ethernet | `drivers/ethernet/eth_dwmac_meson_s4.c` | ⚠️ Development |
| USB XHCI | None (DTS only) | ❌ Not implemented |

## Quick Build Reference

```bash
# Build hello_world
west build -b s905y4_2g -d build_s4 -s zephyr/samples/hello_world --pristine

# Package uImage (automated by board CMakeLists.txt)
mkimage -A arm64 -O u-boot -T standalone -C none \
    -a 0x01000000 -e 0x01000000 \
    -n "Zephyr S4 S905Y4" \
    -d build_s4/zephyr/zephyr.bin build_s4/zephyr/zephyr.uimg

# Deploy to SD card
bash boards/amlogic/s905y4_2g/deploy_sd.sh build_s4 /dev/sdb1
```

See `boards/amlogic/s905y4_2g/BOOT.md` for full boot documentation.