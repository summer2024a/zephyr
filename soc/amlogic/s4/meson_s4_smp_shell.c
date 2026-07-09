/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * S4 SMP status shell command (PSCI-based CPU boot).
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "meson_s4_smp_boot.h"

static int cmd_meson_s4_smp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if defined(CONFIG_SMP_BOOT_DELAY)
	if (!meson_s4_smp_boot_done()) {
		shell_print(sh, "this CPU id=%u MPIDR=0x%llx MPIDR_TO_CORE=0x%llx",
			    arch_curr_cpu()->id,
			    (unsigned long long)GET_MPIDR(),
			    (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()));
		shell_print(sh, "Booting secondary CPUs (z_smp_init)...");
		if (meson_s4_smp_boot_now() != 0) {
			shell_error(sh, "z_smp_init failed, arch_num_cpus()=%u",
				    arch_num_cpus());
			return -EIO;
		}
	} else {
		shell_print(sh, "Secondary CPUs already online (skip z_smp_init)");
	}
#endif

	shell_print(sh, "CONFIG_SMP=%s MP_MAX=%u arch_num_cpus()=%u",
		    IS_ENABLED(CONFIG_SMP) ? "y" : "n",
		    (unsigned int)CONFIG_MP_MAX_NUM_CPUS, arch_num_cpus());
	shell_print(sh, "this CPU id=%u MPIDR=0x%llx MPIDR_TO_CORE=0x%llx",
		    arch_curr_cpu()->id,
		    (unsigned long long)GET_MPIDR(),
		    (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()));
	shell_print(sh, "Boot method: PSCI (arm,psci-1.0 smc)");

	if (arch_num_cpus() >= 4U) {
		shell_print(sh, "OK: %u CPUs online (PSCI CPU_ON)",
			    arch_num_cpus());
	} else {
		shell_print(sh, "WARN: expected 4 CPUs, got %u", arch_num_cpus());
	}

	return 0;
}

SHELL_CMD_REGISTER(meson_s4_smp, NULL, "S4 SMP / PSCI status", cmd_meson_s4_smp);
SHELL_CMD_REGISTER(s4_smp, NULL, "Alias of meson_s4_smp", cmd_meson_s4_smp);
