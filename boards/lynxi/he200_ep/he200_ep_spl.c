/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPL loads zephyr.bin as u-boot.bin (entry 0x800100000). Match RT-Thread he200
 * bring-up that full U-Boot used to perform.
 */

#include <stdint.h>

/* Lynxi CPR / sysctl — same offsets as RT-Thread drv_sysctl_lite.c */
#define HE200_CPR_BASE          0x12500000U

static inline void he200_sysctl_gate_on(uint32_t reg_off, uint32_t bit)
{
	volatile uint32_t *reg = (volatile uint32_t *)(HE200_CPR_BASE + reg_off);
	uint32_t v = *reg;

	*reg = v | (1U << bit);
}

void he200_ep_spl_soc_init(void)
{
	/* UART APB fabric clock (LITE_FABRIC_PCLK2) */
	he200_sysctl_gate_on(0x6cU, 9U);
	/* periph_uart0_sclk */
	he200_sysctl_gate_on(0xb4U, 1U);
}

/*
 * RT-Thread init_cpu_sys clears alignment traps and only enables MMU/cache
 * after early page tables exist. Zephyr z_arm64_el1_init turns on SCTLR.C too
 * early — clear it before z_arm64_mm_init().
 */
void he200_ep_spl_el1_fixup(void)
{
	uint64_t sctlr;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	/* A (bit 1) + SA (bit 3): match RT-Thread entry_point.S */
	sctlr &= ~((1ULL << 1) | (1ULL << 3));
	/* M + C: SPL may leave EL1 MMU on; Zephyr builds fresh tables in mm_init */
	sctlr &= ~((1ULL << 0) | (1ULL << 2));
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
	__asm__ volatile(
		"dsb	ishst\n"
		"tlbi	vmalle1\n"
		"dsb	ish\n"
		"isb\n"
		: : : "memory");
}

void he200_ep_spl_mmu_prepare(void)
{
	he200_ep_spl_el1_fixup();
}

void z_arm64_el_highest_plat_init(void)
{
	he200_ep_spl_soc_init();

#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
	extern void he200_ep_boot_debug_highest(void);

	he200_ep_boot_debug_highest();
#endif
}

void z_arm64_el1_plat_init(void)
{
	he200_ep_spl_el1_fixup();

#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
	extern void he200_ep_boot_marker(char tag);

	he200_ep_boot_marker('L');
#endif
}

#ifdef CONFIG_SOC_PREP_HOOK
void soc_prep_hook(void)
{
#ifdef CONFIG_HE200_EP_EARLY_UART_DEBUG
	extern void he200_ep_boot_marker(char tag);

	he200_ep_boot_marker('P');
#endif
}
#endif
