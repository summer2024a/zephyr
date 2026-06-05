/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static int cmd_ka200_smp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "CONFIG_SMP=%s MP_MAX=%u arch_num_cpus()=%u",
		    IS_ENABLED(CONFIG_SMP) ? "y" : "n",
		    (unsigned int)CONFIG_MP_MAX_NUM_CPUS, arch_num_cpus());
	shell_print(sh, "this CPU id=%u MPIDR=0x%llx",
		    arch_curr_cpu()->id,
		    (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()));

	if (arch_num_cpus() >= 8U) {
		shell_print(sh, "OK: 8 CPUs online (see idle 00..07 in 'kernel thread stacks')");
	} else {
		shell_print(sh, "WARN: expected 8 CPUs, got %u", arch_num_cpus());
	}

	shell_print(sh, "Boot printk 'Secondary CPU core ... is up' needs CONFIG_LOG_PRINTK=n");

	return 0;
}

SHELL_CMD_REGISTER(ka200_smp, NULL, "KA200 SMP / spin-table status", cmd_ka200_smp);
/* Alias for existing scripts / docs */
SHELL_CMD_REGISTER(he200_smp, NULL, "Alias of ka200_smp", cmd_ka200_smp);
