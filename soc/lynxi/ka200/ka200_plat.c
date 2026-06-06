/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * KA200 EL2: enable GICv3 system register access (SPL may leave SRE unset).
 */

#include <stdint.h>

void ka200_boot_marker(char tag);

void z_arm64_el2_plat_init(void)
{
	uint64_t reg;

#ifdef CONFIG_SOC_KA200_BOOT_TRACE
	ka200_boot_marker('G');
#endif

	__asm__ volatile("mrs %0, S3_4_C12_C9_5" : "=r"(reg));

	reg |= (1 << 0);
	reg |= (1 << 3);
	reg |= (1 << 1);
	reg |= (1 << 2);

	__asm__ volatile("msr S3_4_C12_C9_5, %0" : : "r"(reg));
	__asm__ volatile("isb" : : : "memory");

#ifdef CONFIG_SOC_KA200_BOOT_TRACE
	ka200_boot_marker('g');
#endif
}
