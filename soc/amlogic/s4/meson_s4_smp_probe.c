/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional SMP probe for hardware debug (no Shell required).
 */

#include <stdio.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <kernel_internal.h>

extern void meson_s4_boot_marker(char tag);

#if defined(CONFIG_SMP) && defined(CONFIG_SMP_BOOT_DELAY)

void meson_s4_smp_probe_now(void)
{
	unsigned int key;

	meson_s4_boot_marker('Q');
	printf("s4: smp probe MPIDR=0x%llx MPIDR_TO_CORE=0x%llx\n",
	       (unsigned long long)GET_MPIDR(),
	       (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()));

	/*
	 * Let POST_KERNEL drivers (GIC/UART IRQ) settle before PSCI CPU_ON.
	 * z_smp_init runs with IRQs masked inside arch_cpu_start on S4.
	 */
	k_sleep(K_MSEC(100));

	printf("s4: calling z_smp_init()\n");
	meson_s4_boot_marker('+');
	key = arch_irq_lock();
	z_smp_init();
	arch_irq_unlock(key);
	meson_s4_boot_marker('9');
	printf("s4: z_smp_init returned, arch_num_cpus()=%u\n", arch_num_cpus());
}

#endif
