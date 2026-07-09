/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE)
void meson_s4_smp_probe_now(void);
#endif

int main(void)
{
	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);

#if defined(CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE)
	meson_s4_smp_probe_now();
#endif

	return 0;
}
