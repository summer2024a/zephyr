# Lynxi KA200 SoC (Zephyr)

## Scope

`SOC_LYNXI_KA200` covers **one SoC**: 8× Cortex-A, GICv3, CPR, APB peripherals.

Product SKUs such as **KA200_EP** and **KA200_RC** differ **only in PCIe** (endpoint vs root complex, DBI, MSI-X, sysctl gates at CPR `0x84`). UART, eMMC, GMAC, I2C, DMA, SMP spin-table, and MMU device maps are **identical** across SKUs.

## Where to put new code

| Change | Location |
|--------|----------|
| Shared driver / MMU / sysctl / SMP | `soc/lynxi/ka200/` or `drivers/` + `boards/lynxi/common/he200_peripherals.dtsi` |
| PCIe EP or RC only | Board `defconfig` + DTS overlay + dedicated driver Kconfig (`*_KA200_EP` / `*_KA200_RC`) |
| SPL/EL fixup, early UART, EL2 GIC SRE | `ka200_spl.c`, `ka200_plat.c`, `ka200_early_uart.c` |
| eMMC CPR gate + SDHCI host | `sysctl_lite.c`（CPR `0x88`）；驱动见 `drivers/sdhc/sdhc_lynxi_dwcmshc.c` |
| SPL link address only | `boards/lynxi/he200_ep/linker.ld` |

Do **not** fork peripheral drivers under `he200_ep/` for EP SKU.

eMMC on `he200_ep` is **verified** (`disk_access_init` OK, ~29 GB geometry). See [PORTING.md §10.5](../../boards/lynxi/PORTING.md).

Full naming rules: [boards/lynxi/PORTING.md](../../boards/lynxi/PORTING.md) §1.1.
