/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * KA200 has no PSCI firmware. Secondary CPUs are released via the spin-table
 * addresses used by RT-Thread he200 (board.c cpu_release_paddr[]), not the
 * single cpu-release-addr in DTS (boot ROM / SPL convention).
 */

#include <zephyr/cache.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/sys/barrier.h>
#include <errno.h>

/*
 * RT-Thread bsp/lynxi/he200/drivers/board.c — per-logical-CPU release cells.
 * CPU0 is brought up by SPL; indices 1..7 are written then SEV is issued.
 */
static const uintptr_t ka200_cpu_release_paddr[] = {
	[0] = 0x401ff00UL,
	[1] = 0x401ff00UL,
	[2] = 0x401ff08UL,
	[3] = 0x401ff10UL,
	[4] = 0x401ff18UL,
	[5] = 0x401ff20UL,
	[6] = 0x401ff28UL,
	[7] = 0x401ff30UL,
};

static int ka200_cpu_index(unsigned long cpuid)
{
	if (cpuid < 4) {
		return (int)cpuid;
	}
	if (cpuid >= 0x100UL && cpuid <= 0x103UL) {
		return (int)(4 + (cpuid - 0x100UL));
	}
	return -1;
}

int pm_cpu_on(unsigned long cpuid, uintptr_t entry_point)
{
	int idx = ka200_cpu_index(cpuid);
	uintptr_t release_addr;
	volatile uint64_t *release;

	if (idx <= 0) {
		return -EINVAL;
	}
	if (idx >= (int)ARRAY_SIZE(ka200_cpu_release_paddr)) {
		return -EINVAL;
	}

	release_addr = ka200_cpu_release_paddr[idx];
	release = (volatile uint64_t *)release_addr;
	*release = entry_point;

	sys_cache_data_flush_range((void *)release_addr, sizeof(uint64_t));
	barrier_dsync_fence_full();
	__asm__ volatile("sev" ::: "memory");

	return 0;
}

int pm_cpu_off(void)
{
	return -ENOTSUP;
}
