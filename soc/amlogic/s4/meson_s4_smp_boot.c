/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared SMP boot state for auto-probe, shell, and deferred init paths.
 */

#include "meson_s4_smp_boot.h"

#if defined(CONFIG_SMP) && defined(CONFIG_SOC_AMLOGIC_MESON_S4)

#include <stdio.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <kernel_internal.h>

static atomic_t smp_boot_done;

bool meson_s4_smp_boot_done(void)
{
	if (atomic_get(&smp_boot_done) != 0) {
		return true;
	}

	return arch_num_cpus() > 1U;
}

int meson_s4_smp_boot_now(void)
{
	unsigned int key;
	int ret = 0;

	if (meson_s4_smp_boot_done()) {
		return 0;
	}

	/*
	 * Let POST_KERNEL drivers (GIC/UART) settle. arch_cpu_start on S4
	 * masks IRQs during each PSCI CPU_ON handshake.
	 */
	k_sleep(K_MSEC(100));

	key = arch_irq_lock();
	z_smp_init();
	arch_irq_unlock(key);

	if (arch_num_cpus() <= 1U) {
		ret = -EIO;
	} else {
		atomic_set(&smp_boot_done, 1);
	}

	return ret;
}

#endif /* CONFIG_SMP && CONFIG_SOC_AMLOGIC_MESON_S4 */
