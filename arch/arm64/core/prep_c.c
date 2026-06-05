/*
 * Copyright (c) 2019 Carlo Caione <ccaione@baylibre.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Full C support initialization
 *
 * Initialization of full C support: zero the .bss and call z_cstart().
 *
 * Stack is available in this module, but not the global data/bss until their
 * initialization is performed.
 */

#include "kernel_arch_func.h"

#include <zephyr/linker/linker-defs.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/arch/cache.h>
#include <zephyr/arch/common/xip.h>
#include <zephyr/arch/common/init.h>

extern void z_arm64_mm_init(bool is_primary_core);

__weak void z_arm64_mm_init(bool is_primary_core) { }

#if defined(CONFIG_SOC_LYNXI_KA200)
void ka200_spl_mmu_prepare(void);
#endif

#if defined(CONFIG_SOC_LYNXI_KA200_EARLY_UART_DEBUG)
extern void ka200_prep_trace(char step);
#else
static inline void ka200_prep_trace(char step) { ARG_UNUSED(step); }
#endif

/**
 *
 * @brief Prepare to and run C code
 *
 * This routine prepares for the execution of and runs C code.
 *
 */
FUNC_NORETURN void z_prep_c(void)
{
	soc_prep_hook();

	/* Initialize tpidrro_el0 with our struct _cpu instance address */
	write_tpidrro_el0((uintptr_t)&_kernel.cpus[0]);
	ka200_prep_trace('t');

	arch_bss_zero();
	ka200_prep_trace('b');
	arch_data_copy();
	ka200_prep_trace('d');
#ifdef CONFIG_ARM64_SAFE_EXCEPTION_STACK
	/* After bss clean, _kernel.cpus is in bss section */
	z_arm64_safe_exception_stack_init();
#endif
#if defined(CONFIG_SOC_LYNXI_KA200)
	ka200_spl_mmu_prepare();
#endif
	ka200_prep_trace('D');
	z_arm64_mm_init(true);
	ka200_prep_trace('m');

	z_arm64_interrupt_init();
	ka200_prep_trace('i');

	z_cstart();
	CODE_UNREACHABLE;
}


#if CONFIG_MP_MAX_NUM_CPUS > 1
extern FUNC_NORETURN void arch_secondary_cpu_init(void);
void z_arm64_secondary_prep_c(void)
{
	arch_secondary_cpu_init();
#if CONFIG_ARCH_CACHE
	arch_cache_init();
#endif

	CODE_UNREACHABLE;
}
#endif
