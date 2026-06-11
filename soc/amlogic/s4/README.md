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

- BL31 (ARM Trusted Firmware) runs at EL3 and uses PSCI for CPU_ON
- BL33 (U-Boot or Zephyr) enters at 0x01000000
- Secondary CPUs are brought up via PSCI CPU_ON SMC calls

## Zephyr Configuration

Key Kconfig options:
- `CONFIG_SOC_AMLOGIC_MESON_S4`: Select S4 SoC
- `CONFIG_SOC_MESON_S4_BOOT_TRACE`: Enable early UART boot markers
- `CONFIG_SOC_MESON_S4_SHELL_SMP`: SMP status shell command
- `CONFIG_ARM_PSCI`: Use PSCI for secondary CPU boot