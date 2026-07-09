/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional SMP probe for hardware debug (no Shell required).
 */

#include <stdio.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>

#include "meson_s4_smp_boot.h"

extern void meson_s4_boot_marker(char tag);

#if defined(CONFIG_SMP) && defined(CONFIG_SMP_BOOT_DELAY)

void meson_s4_smp_probe_now(void)
{
	meson_s4_boot_marker('Q');
	printf("s4: smp probe MPIDR=0x%llx MPIDR_TO_CORE=0x%llx\n",
	       (unsigned long long)GET_MPIDR(),
	       (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()));

	printf("s4: calling z_smp_init()\n");
	meson_s4_boot_marker('+');
	if (meson_s4_smp_boot_now() != 0) {
		printf("s4: z_smp_init failed, arch_num_cpus()=%u\n", arch_num_cpus());
	} else {
		meson_s4_boot_marker('9');
		printf("s4: z_smp_init returned, arch_num_cpus()=%u\n", arch_num_cpus());
	}
}

#endif
