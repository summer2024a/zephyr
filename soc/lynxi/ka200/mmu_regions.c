/*
 * Copyright 2024 Lynxi Technologies Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
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

	/* GIC Redistributor */
	MMU_REGION_FLAT_ENTRY("GIC_REDIST",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 1),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 1),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* Spin-table release cells (RT cpu_release_paddr[], 0x401ff00..0x401ff30) */
	MMU_REGION_FLAT_ENTRY("SPIN_TABLE",
			      0x401ff00UL,
			      0x40UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* UART0 + CPR */
	MMU_REGION_FLAT_ENTRY("UART0",
			      0x10006000UL,
			      0x1000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	MMU_REGION_FLAT_ENTRY("CPR",
			      0x12500000UL,
			      0x10000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* APB peripherals (GPIO / I2C / SPI / DMA) */
	MMU_REGION_FLAT_ENTRY("SOC_APB",
			      0x10002000UL,
			      0x0003e000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* GMAC DWC QoS @ 0x10020000（与 eMMC 一样单独映射，避免 SOC_APB 区访问异常） */
	MMU_REGION_FLAT_ENTRY("GMAC",
			      0x10020000UL,
			      0x00010000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* IO pinmux / pinctrl (lynxi,lite-pinctrl @ 0x12000000) */
	MMU_REGION_FLAT_ENTRY("PINCTRL",
			      0x12000000UL,
			      0x00010000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

	/* eMMC DWC MSHC @ 0x10040000 */
	MMU_REGION_FLAT_ENTRY("EMMC",
			      0x10040000UL,
			      0x00040000UL,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
