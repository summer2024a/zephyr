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

The board defconfig (`s905y4_2g_defconfig`) already enables Shell, SMP,
logging, and kernel shell commands. Building with the hello_world sample
produces a Shell Console application with interactive SMP/device/kernel
commands — no separate app directory needed.

```bash
west build -b s905y4_2g -d build_s4_shell -s zephyr/samples/hello_world --pristine
```

Both builds automatically generate `zephyr.uimg` (mkimage uImage) if
`u-boot-tools` is installed, thanks to the board CMakeLists.txt
`extra_post_build_commands` step. uImage parameters:

```
Load Address:  0x01000000  (= BL33 NS_BL33_ENTRYPOINT)
Entry Point:   0x01000000  (load = run, no relocation needed)
Image Type:    AArch64 U-Boot Standalone Program (uncompressed)
```

### Deploy

```bash
# One-command SD card deployment (auto-package + copy)
bash boards/amlogic/s905y4_2g/deploy_sd.sh build_s4_shell /dev/sdb1

# Or manually:
cp build_s4_shell/zephyr/zephyr.uimg /mnt/sdcard/
sync
```

### Boot (U-Boot serial console)

```
fatload mmc 1 0x01000000 zephyr.uimg
bootm 0x01000000
```

Serial console: UART_B, 115200, 8N1, GPIOB_0/GPIOB_1.

### Shell commands available after boot

```
uart:~$ meson_s4_smp          # SMP/PSCI status (4 cores)
uart:~$ kernel threads        # Thread list
uart:~$ device list            # Device tree enumeration
uart:~$ log status             # Logging configuration
uart:~$ reboot                 # Reboot via PSCI
```

See `boards/amlogic/s905y4_2g/BOOT.md` for full boot and packaging documentation.