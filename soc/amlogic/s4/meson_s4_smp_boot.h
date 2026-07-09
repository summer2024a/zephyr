/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MESON_S4_SMP_BOOT_H_
#define MESON_S4_SMP_BOOT_H_

#include <stdbool.h>
#include <errno.h>

#if defined(CONFIG_SMP) && defined(CONFIG_SOC_AMLOGIC_MESON_S4)

/** True after a successful z_smp_init or when arch_num_cpus() > 1. */
bool meson_s4_smp_boot_done(void);

/**
 * Boot secondary CPUs once (PSCI CPU_ON + arch_cpu_start sync).
 * Safe to call from main(), shell, or a worker thread; no-op if already done.
 *
 * @return 0 on success or already booted, negative errno on failure.
 */
int meson_s4_smp_boot_now(void);

#else

static inline bool meson_s4_smp_boot_done(void)
{
	return false;
}

static inline int meson_s4_smp_boot_now(void)
{
	return -ENOTSUP;
}

#endif

#endif /* MESON_S4_SMP_BOOT_H_ */
