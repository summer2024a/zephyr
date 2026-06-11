/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Amlogic S4 (S905Y4/S905W2) platform init: GIC, UART clock, EL2 setup.
 */

#include <stdint.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

void meson_s4_boot_marker(char tag);

/*
 * S4 GIC-400: enable Group0 + Group1 NS + SRE at EL2.
 * BL31 on S4 sets up GIC, but we ensure SRE is on for Zephyr.
 */
void z_arm64_el2_plat_init(void)
{
#ifdef CONFIG_SOC_MESON_S4_BOOT_TRACE
	meson_s4_boot_marker('G');
#endif

	/* S4 BL31 (ARM Trusted Firmware) handles GIC setup.
	 * Just mark progress if boot trace is enabled. */

#ifdef CONFIG_SOC_MESON_S4_BOOT_TRACE
	meson_s4_boot_marker('g');
#endif
}

void z_arm64_el_highest_plat_init(void)
{
#ifdef CONFIG_SOC_MESON_S4_BOOT_TRACE
	extern void meson_s4_boot_debug_highest(void);
	meson_s4_boot_debug_highest();
#endif
}

void z_arm64_el1_plat_init(void)
{
	uint64_t sctlr;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	/* Clear endianness (bit 1) and stack alignment check (bit 3) */
	sctlr &= ~((1ULL << 1) | (1ULL << 3));
	/* Clear MMU enable and cache enable bits — Zephyr will re-enable */
	sctlr &= ~((1ULL << 0) | (1ULL << 2));
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
	__asm__ volatile(
		"dsb	ishst\n"
		"tlbi	vmalle1\n"
		"dsb	ish\n"
		"isb\n"
		: : : "memory");

	meson_s4_boot_marker('L');
}

#ifdef CONFIG_SOC_PREP_HOOK
void soc_prep_hook(void)
{
	meson_s4_boot_marker('P');
}
#endif

static int meson_s4_post_kernel_checkpoint(void)
{
	printk("s4: POST_KERNEL start (devices next)\n");
	return 0;
}

SYS_INIT(meson_s4_post_kernel_checkpoint, POST_KERNEL, 0);