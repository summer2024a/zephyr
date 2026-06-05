/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_SDHC_LYNXI_SYSCTL_H_
#define ZEPHYR_INCLUDE_SDHC_LYNXI_SYSCTL_H_

#include <stdint.h>

void lynxi_sysctl_gate_enable(uint32_t reg_off, uint32_t bit);
void lynxi_sysctl_uart0_enable(void);
void lynxi_sysctl_emmc_enable(void);

#endif
