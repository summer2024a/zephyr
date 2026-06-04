/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/kernel.h>

extern void z_arm64_el2_plat_init(void);

#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
void he200_ep_boot_marker(char tag);
#endif

void z_arm64_el2_plat_init(void)
{
#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
	he200_ep_boot_marker('G');
#endif
	/*
	 * 确保在 EL2 级别启用 GICv3 系统寄存器访问权限
	 * 这是针对 he200 板卡的特定修复，因为 U-Boot 可能没有正确配置 EL2 访问权限
	 * 没有这个配置，从核可能会在启动时卡住
	 */
	uint64_t reg;

	/* 使用内联汇编直接访问系统寄存器 */
	__asm__ volatile (
		"mrs %0, S3_4_C12_C9_5" : "=r" (reg)
	);

	/* 启用系统寄存器接口 */
	reg |= (1 << 0); /* Set SRE (System Register Enable) */
	
	/* 启用对 ICC_SRE_EL1 的访问 */
	reg |= (1 << 3); /* Set EN (Enable lower exception level access) */
	
	/* 确保 FIQ/IRQ 不被绕过（可选，但推荐） */
	reg |= (1 << 1); /* Set DFB (Disable FIQ Bypass) */
	reg |= (1 << 2); /* Set DIB (Disable IRQ Bypass) */
	
	/* 写入寄存器 */
	__asm__ volatile (
		"msr S3_4_C12_C9_5, %0" : : "r" (reg)
	);
	
	/* 确保配置立即生效 */
	__asm__ volatile ("isb" : : : "memory");

#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
	he200_ep_boot_marker('g');
#endif
}