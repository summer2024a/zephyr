/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMU region mappings for Amlogic S4 (S905Y4/S905W2).
 *
 * Key peripheral addresses from meson-s4.dtsi:
 *   GIC-400:      0xFFF01000
 *   UART_B:       0xFE07A000  (APB bus 0xFE000000)
 *   MMC (SD):     0xFE08A000
 *   MMC (eMMC):   0xFE08C000
 *   ETH (DW MAC): 0xFDC00000
 *   USB ctrl:     0xFE03A000
 *   XHCI:         0xFDE00000
 *   Clock/reset:  0xFE002000
 *   MDIO mux:     0xFE0028000 (within APB bus)
 *
 * BL33 entry: 0x01000000 (kernel RAM starts here)
 */

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/arch/arm64/arm_mmu.h>

static const struct arm_mmu_region mmu_regions[] = {
	/* GIC Distributor */
	MMU_REGION_FLAT_ENTRY("GIC_DIST",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 0),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 0),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* GIC CPU Interface + Redistributor */
	MMU_REGION_FLAT_ENTRY("GIC_CPU",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 1),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 1),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* GIC VCPU + VIF */
	MMU_REGION_FLAT_ENTRY("GIC_VCPU",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 2),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 2),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* GIC Virtual Interface */
	MMU_REGION_FLAT_ENTRY("GIC_VIF",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 3),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 3),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* APB4 peripheral bus (UART, clocks, reset, pinctrl, I2C, SPI, PWM, MDIO mux) */
	MMU_REGION_FLAT_ENTRY("APB4_BUS",
			      0xFE000000UL,
			      0x480000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* MMC controllers (SD + eMMC) — 0xFE08A000..0xFE08D000 */
	MMU_REGION_FLAT_ENTRY("MMC",
			      0xFE08A000UL,
			      0x4000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* Ethernet DW MAC @ 0xFDC00000 */
	MMU_REGION_FLAT_ENTRY("ETH",
			      0xFDC00000UL,
			      0x10000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* USB control @ 0xFE03A000 */
	MMU_REGION_FLAT_ENTRY("USB_CTRL",
			      0xFE03A000UL,
			      0x100UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* XHCI @ 0xFDE00000 */
	MMU_REGION_FLAT_ENTRY("XHCI",
			      0xFDE00000UL,
			      0x100000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* USB2 PHY 0 @ 0xFE003C000 and PHY 1 @ 0xFE003E000 (within APB4 bus) */
	/* Already covered by APB4_BUS mapping above */
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};