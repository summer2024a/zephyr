/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sdhc/lynxi_sysctl.h>

#define LYNXI_CPR_BASE         0x12500000UL
#define LYNXI_PINCTRL_BASE     0x12000000UL
#define LYNXI_PINCFG0_OFF   0x44U
#define LYNXI_PINCFG_STEP   0x4U
#define LYNXI_PINMUX_GPIO   BIT(10)

/* dt-bindings/reset/lynchip-lite-resets.h: LITE_GMAC_R -> CPR 0x8c bit0 */
#define LYNXI_GMAC_CPR_RST_BIT    0U
/* clk-lynxi-lite.c: LITE_GMAC_ACLK / LITE_GMAC_HCLK / LITE_ETH_PHY @ 0x8c */
#define LYNXI_GMAC_CPR_ACLK_BIT   1U
#define LYNXI_GMAC_CPR_HCLK_BIT   2U
#define LYNXI_GMAC_CPR_ETHPHY_BIT 9U
void lynxi_sysctl_gate_enable(uint32_t reg_off, uint32_t bit)
{
	volatile uint32_t *reg = (volatile uint32_t *)(LYNXI_CPR_BASE + reg_off);
	uint32_t v = *reg;

	*reg = v | BIT(bit);
}

static void lynxi_sysctl_gate_clear(uint32_t reg_off, uint32_t bit)
{
	volatile uint32_t *reg = (volatile uint32_t *)(LYNXI_CPR_BASE + reg_off);
	uint32_t v = *reg;

	*reg = v & ~BIT(bit);
}

static void lynxi_sysctl_gmac_reset_pulse(void)
{
	/*
	 * reset-lynchip-lite.c: assert=clear bit, deassert=set bit.
	 * LITE_GMAC_R (1120) @ CPR 0x8c bit0 — stmmac_dvr_probe() 亦会
	 * reset_control_assert/deassert(stmmac_rst)（若 DTS 提供）。
	 */
	lynxi_sysctl_gate_clear(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				LYNXI_GMAC_CPR_RST_BIT);
	k_busy_wait(100);
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_RST_BIT);
	k_busy_wait(1000);
}

uint32_t lynxi_sysctl_cpr_read(uint32_t reg_off)
{
	volatile uint32_t *reg = (volatile uint32_t *)(LYNXI_CPR_BASE + reg_off);

	return *reg;
}

void lynxi_sysctl_reset_deassert(uint32_t reg_off, uint32_t bit)
{
	lynxi_sysctl_gate_enable(reg_off, bit);
}

void lynxi_sysctl_uart0_enable(void)
{
	lynxi_sysctl_gate_enable(0x6cU, 9U);
	lynxi_sysctl_gate_enable(0xb4U, 1U);
}

void lynxi_sysctl_emmc_enable(void)
{
	lynxi_sysctl_gate_enable(0x88U, 1U);
	lynxi_sysctl_gate_enable(0x88U, 2U);
	lynxi_sysctl_gate_enable(0x88U, 3U);
}

void lynxi_sysctl_gpio_enable(void)
{
	/*
	 * clk-lynxi-lite.c: LITE_PERIPH_GPIO_DB @ 0x90 bit1,
	 * LITE_PERIPH_GPIO_INTR @ 0x90 bit2。lynchip-lite-base.dtsi gpio
	 * 节点无 resets/clocks，gpio-dwapb 仅用 optional reset（通常为 NULL）。
	 */
	lynxi_sysctl_gate_enable(0x90U, 1U);
	lynxi_sysctl_gate_enable(0x90U, 2U);
}

void lynxi_sysctl_fabric_apb_enable(void)
{
	lynxi_sysctl_gate_enable(0x6cU, 5U);
	lynxi_sysctl_gate_enable(0x6cU, 9U);
}

void lynxi_gpio_dw_block_init(void)
{
	lynxi_sysctl_fabric_apb_enable();
	lynxi_sysctl_gpio_enable();
}

void lynxi_pinctrl_pin_gpio_mode(unsigned int pin_index)
{
	/*
	 * pinctrl-lynlite.c: lynxi_pmx_set_mode() bit10=1 → GPIO，
	 * bit10=0 → normal（PD23 默认为 ETH_PHY_CLK，PD24 为 ETH_PHY_RSTN）。
	 */
	volatile uint32_t *reg = (volatile uint32_t *)(LYNXI_PINCTRL_BASE +
						     LYNXI_PINCFG0_OFF +
						     pin_index * LYNXI_PINCFG_STEP);
	uint32_t val = *reg;

	*reg = val | LYNXI_PINMUX_GPIO;
}

void lynxi_gpio_pin_prepare_output(unsigned int pinctrl_idx, unsigned int port,
				   unsigned int pin)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pin);

	lynxi_gpio_dw_block_init();
	lynxi_pinctrl_pin_gpio_mode(pinctrl_idx);
}

void lynxi_sysctl_gmac_probe_clocks(void)
{
	/*
	 * dwc_qos_probe(): 先 aclk，再 phy_ref_clk；不写 0x66f（链路建立后
	 * lynchip_lite_cpr_gmac_config() 才写）。LITE_GMAC_R 脉冲对齐
	 * stmmac_dvr_probe() reset_control_assert/deassert。
	 */
	lynxi_sysctl_fabric_apb_enable();
	lynxi_sysctl_gmac_reset_pulse();
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_ACLK_BIT);
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_ETHPHY_BIT);
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_HCLK_BIT);
}

void lynxi_sysctl_gmac_aux_clocks_enable(void)
{
	/*
	 * lynchip-lite-cpr.dtsi &eth clock-names "aclk","phy_ref_clk" —
	 * dwc_qos_probe() 会 enable；MDIO/PHY 前打开。
	 */
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_ACLK_BIT);
	lynxi_sysctl_gate_enable(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF,
				 LYNXI_GMAC_CPR_ETHPHY_BIT);
}

void lynxi_sysctl_gmac_cpr_speed_set(unsigned int speed_mbps)
{
	uint32_t value;

	switch (speed_mbps) {
	case 1000:
		value = LYNXI_SYSCTL_GMAC_CTRL_1000M;
		break;
	case 100:
		value = LYNXI_SYSCTL_GMAC_CTRL_100M;
		break;
	case 10:
		value = LYNXI_SYSCTL_GMAC_CTRL_10M;
		break;
	default:
		value = LYNXI_SYSCTL_GMAC_CTRL_1000M;
		break;
	}

	lynxi_sysctl_gmac_ctrl_set(value);
}

void lynxi_sysctl_gmac_cpr_apply(void)
{
	printk("he200 GMAC: lynchip_lite_cpr_gmac_config 0x66f\n");
	lynxi_sysctl_gmac_cpr_speed_set(1000);
	printk("he200 GMAC: gmac-ctrl 0x8c=0x%08x\n",
	       lynxi_sysctl_cpr_read(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF));
}

void lynxi_sysctl_gmac_hw_prepare(void)
{
	lynxi_sysctl_gmac_probe_clocks();
	lynxi_sysctl_gmac_aux_clocks_enable();
	lynxi_sysctl_gmac_cpr_apply();
}

void lynxi_sysctl_gmac_enable(void)
{
	lynxi_sysctl_gmac_hw_prepare();
}

void lynxi_sysctl_gmac_ctrl_set(uint32_t value)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)(LYNXI_CPR_BASE + LYNXI_SYSCTL_GMAC_CTRL_REG_OFF);

	*reg = value;
}

#if defined(CONFIG_GPIO) && defined(CONFIG_GPIO_DW)
#include <zephyr/init.h>

static int ka200_gpio_sysctl_init(void)
{
	lynxi_gpio_dw_block_init();
	return 0;
}

SYS_INIT(ka200_gpio_sysctl_init, PRE_KERNEL_1, 0);
#endif
