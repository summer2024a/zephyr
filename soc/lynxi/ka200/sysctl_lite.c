/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/sys/util.h>

#define LYNXI_CPR_BASE 0x12500000UL

void lynxi_sysctl_gate_enable(uint32_t reg_off, uint32_t bit)
{
	volatile uint32_t *reg = (volatile uint32_t *)(LYNXI_CPR_BASE + reg_off);
	uint32_t v = *reg;

	*reg = v | BIT(bit);
}

void lynxi_sysctl_uart0_enable(void)
{
	/* RT drv_sysctl_lite.c: LITE_FABRIC_PCLK2 + periph_uart0_sclk */
	lynxi_sysctl_gate_enable(0x6cU, 9U);
	lynxi_sysctl_gate_enable(0xb4U, 1U);
}

void lynxi_sysctl_emmc_enable(void)
{
	/* RT drv_sysctl_lite.c: emmc_aclk / emmc_hclk / emmc_cclk @ 0x88 */
	lynxi_sysctl_gate_enable(0x88U, 1U);
	lynxi_sysctl_gate_enable(0x88U, 2U);
	lynxi_sysctl_gate_enable(0x88U, 3U);
}
