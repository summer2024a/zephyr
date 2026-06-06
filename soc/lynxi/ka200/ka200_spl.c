/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * KA200 SPL / U-Boot handoff: CPR clocks, EL1 SCTLR fixup (all HE200 boards).
 */

#include <stdint.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sdhc/lynxi_sysctl.h>

void ka200_boot_marker(char tag);

void ka200_spl_soc_init(void)
{
	lynxi_sysctl_uart0_enable();
}

void ka200_spl_el1_fixup(void)
{
	uint64_t sctlr;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr &= ~((1ULL << 1) | (1ULL << 3));
	sctlr &= ~((1ULL << 0) | (1ULL << 2));
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
	__asm__ volatile(
		"dsb	ishst\n"
		"tlbi	vmalle1\n"
		"dsb	ish\n"
		"isb\n"
		: : : "memory");
}

void ka200_spl_mmu_prepare(void)
{
	ka200_spl_el1_fixup();
}

void z_arm64_el_highest_plat_init(void)
{
	ka200_spl_soc_init();

#ifdef CONFIG_SOC_KA200_BOOT_TRACE
	extern void ka200_boot_debug_highest(void);

	ka200_boot_debug_highest();
#endif
}

void z_arm64_el1_plat_init(void)
{
	ka200_spl_el1_fixup();
	ka200_boot_marker('L');
}

#ifdef CONFIG_SOC_PREP_HOOK
void soc_prep_hook(void)
{
	ka200_spl_soc_init();
	ka200_boot_marker('P');
}
#endif

static int ka200_post_kernel_checkpoint(void)
{
	printk("he200: POST_KERNEL start (devices next)\n");
	return 0;
}

SYS_INIT(ka200_post_kernel_checkpoint, POST_KERNEL, 0);
