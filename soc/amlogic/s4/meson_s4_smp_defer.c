/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deferred SMP init (SMP_BOOT_DELAY=y path).
 */

#include <stdio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <kernel_internal.h>

#if defined(CONFIG_SMP) && defined(CONFIG_SMP_BOOT_DELAY)

#define SMP_DEFER_STACK_SIZE 4096

static K_THREAD_STACK_DEFINE(smp_defer_stack, SMP_DEFER_STACK_SIZE);
static struct k_thread smp_defer_thread;

static void smp_defer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sleep(K_SECONDS(3));
	printf("s4: deferred z_smp_init (cpus=%u before)\n", arch_num_cpus());
	z_smp_init();
	printf("s4: deferred z_smp_init done (cpus=%u)\n", arch_num_cpus());
}

static int smp_defer_start(void)
{
	printf("s4: spawn deferred SMP thread (3s)\n");
	k_thread_create(&smp_defer_thread, smp_defer_stack,
			K_THREAD_STACK_SIZEOF(smp_defer_stack),
			smp_defer_fn, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&smp_defer_thread, "s4_smp_defer");
	return 0;
}

SYS_INIT(smp_defer_start, APPLICATION, 99);

#endif
