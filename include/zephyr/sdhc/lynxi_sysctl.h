/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_SDHC_LYNXI_SYSCTL_H_
#define ZEPHYR_INCLUDE_SDHC_LYNXI_SYSCTL_H_

#include <stdint.h>

#define LYNXI_SYSCTL_GMAC_CTRL_REG_OFF  0x8cu
#define LYNXI_SYSCTL_GMAC_CTRL_1000M    0x66fu
#define LYNXI_SYSCTL_GMAC_CTRL_100M     0x65fu
#define LYNXI_SYSCTL_GMAC_CTRL_10M      0x64fu

/*
 * lynchip-lite-evb.dts reset-gpios = <&portd 23>: PD23 in
 * drivers/pinctrl/pinctrl-lynlite.c (not 3*32+23).
 */
#define LYNXI_PINCTRL_PD23              118U

void lynxi_sysctl_gate_enable(uint32_t reg_off, uint32_t bit);
uint32_t lynxi_sysctl_cpr_read(uint32_t reg_off);
void lynxi_sysctl_reset_deassert(uint32_t reg_off, uint32_t bit);
void lynxi_sysctl_uart0_enable(void);
void lynxi_sysctl_emmc_enable(void);
void lynxi_sysctl_gpio_enable(void);
void lynxi_sysctl_fabric_apb_enable(void);
void lynxi_gpio_dw_block_init(void);
void lynxi_pinctrl_pin_gpio_mode(unsigned int pin_index);
void lynxi_gpio_pin_prepare_output(unsigned int pinctrl_idx, unsigned int port,
				   unsigned int pin);
void lynxi_sysctl_gmac_probe_clocks(void);
void lynxi_sysctl_gmac_aux_clocks_enable(void);
void lynxi_sysctl_gmac_cpr_speed_set(unsigned int speed_mbps);
void lynxi_sysctl_gmac_cpr_apply(void);
void lynxi_sysctl_gmac_hw_prepare(void);
void lynxi_sysctl_gmac_enable(void);
void lynxi_sysctl_gmac_ctrl_set(uint32_t value);

#endif
