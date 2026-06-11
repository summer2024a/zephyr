/*
 * Amlogic S4 (S905Y4/S905W2) platform glue for Synopsys DesignWare MAC 3.70a
 *
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * S4 uses DW MAC 3.70a with internal RMII PHY via MDIO mux.
 * Unlike KA200 (RGMII external PHY), S4 has:
 *   - Internal 10/100 PHY (ethernet-phy-id0180.3301)
 *   - MDIO mux @ 0xFE0028000 for internal/external MDIO selection
 *   - RMII mode (not RGMII)
 *   - Max speed: 100Mbps (internal PHY limitation)
 *
 * ETH base: 0xFDC00000 (MAC registers)
 * ETH regs2: 0xFE024000 (clock/reset control)
 */

#define LOG_MODULE_NAME meson_s4_dwmac
#define LOG_LEVEL CONFIG_ETH_DWMAC_MESON_S4_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define DT_DRV_COMPAT amlogic_meson_axg_dwmac

#include <zephyr/kernel/mm.h>
#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/phy.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "eth_dwmac_priv.h"

/* S4 clock/reset registers (within APB4 bus) */
#define S4_ETH_CLK_CTRL_BASE    0xFE024000UL
#define S4_MDIO_MUX_BASE        0xFE002800UL

/* MDIO mux register offsets */
#define S4_MDIO_MUX_CTRL        0x00U
#define S4_MDIO_MUX_MASK        0x04U

/* ETH clock control register bits */
#define S4_ETH_CLK_TX_EN        (1U << 0)
#define S4_ETH_CLK_RX_EN        (1U << 1)
#define S4_ETH_CLK_PHY_REF_EN   (1U << 2)

/* S4 GIC-400 Distributor base for IRQ type config */
#define S4_GIC_DIST_BASE        0xFFF01000UL
#define S4_GICD_ICFGR_OFF       0xC00UL
#define S4_GIC_ICFGR_LEVEL_ACTIVE_HIGH 3U

static inline uint32_t s4_lo32(uintptr_t val)
{
	return (uint32_t)val;
}

#if DT_NODE_HAS_STATUS(DT_INST_PHANDLE(0, phy_handle), okay)
#define S4_DWMAC_HAS_PHY 1
#define S4_DWMAC_PHY_DEV DEVICE_DT_GET(DT_INST_PHANDLE(0, phy_handle))
#else
#define S4_DWMAC_HAS_PHY 0
#endif

/* Enable internal MDIO mux path to reach internal PHY */
static void s4_dwmac_mdio_mux_enable(void)
{
	volatile uint32_t *mux_ctrl = (volatile uint32_t *)(S4_MDIO_MUX_BASE + S4_MDIO_MUX_CTRL);
	volatile uint32_t *mux_mask = (volatile uint32_t *)(S4_MDIO_MUX_BASE + S4_MDIO_MUX_MASK);

	/* Select internal MDIO (register value 1) */
	*mux_ctrl = 1;
	/* Enable internal MDIO path */
	*mux_mask = 1;

	printk("s4 ETH: MDIO mux enabled (internal path)\n");
}

/* Enable ETH clocks */
static void s4_dwmac_clock_enable(void)
{
	volatile uint32_t *clk_ctrl = (volatile uint32_t *)S4_ETH_CLK_CTRL_BASE;

	/* Enable TX, RX, and PHY reference clocks */
	*clk_ctrl |= S4_ETH_CLK_TX_EN | S4_ETH_CLK_RX_EN | S4_ETH_CLK_PHY_REF_EN;

	printk("s4 ETH: clocks enabled\n");
}

static void s4_dwmac_apply_link_speed(struct dwmac_priv *p,
				      enum phy_link_speed speed)
{
	uint32_t conf = REG_READ(MAC_CONF);

	conf &= ~(MAC_CONF_PS | MAC_CONF_FES | MAC_CONF_DM);

	if (PHY_LINK_IS_FULL_DUPLEX(speed)) {
		conf |= MAC_CONF_DM;
	}

	/* S4 internal PHY max speed = 100Mbps */
	if (PHY_LINK_IS_SPEED_100M(speed)) {
		conf |= MAC_CONF_PS | MAC_CONF_FES;
		printk("s4 ETH: 100Mbps link\n");
	} else {
		conf |= MAC_CONF_PS;
		printk("s4 ETH: 10Mbps link\n");
	}

	REG_WRITE(MAC_CONF, conf);
}

#if S4_DWMAC_HAS_PHY
static void s4_dwmac_phy_link_changed(const struct device *phy_dev,
				       struct phy_link_state *state,
				       void *user_data)
{
	const struct device *eth_dev = user_data;
	struct dwmac_priv *p = eth_dev->data;
	struct net_if *iface = p->iface;

	if (state->is_up) {
		s4_dwmac_apply_link_speed(p, state->speed);
		net_if_carrier_on(iface);
		printk("s4 ETH: link UP, speed=%s\n",
		       PHY_LINK_IS_SPEED_100M(state->speed) ? "100M" : "10M");
	} else {
		net_if_carrier_off(iface);
		printk("s4 ETH: link DOWN\n");
	}
}
#endif

static void s4_gmac_irq_level_high(unsigned int intid)
{
	unsigned int idx = intid / 16U;
	unsigned int shift = (intid % 16U) * 2U;
	mem_addr_t icfgr = S4_GIC_DIST_BASE + S4_GICD_ICFGR_OFF + idx * 4U;
	uint32_t val = sys_read32(icfgr);

	val &= ~(BIT_MASK(2) << shift);
	val |= (S4_GIC_ICFGR_LEVEL_ACTIVE_HIGH << shift);
	sys_write32(val, icfgr);
}

void dwmac_platform_irq_enable(const struct device *dev)
{
	ARG_UNUSED(dev);

	s4_gmac_irq_level_high(DT_INST_IRQN(0));

	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), dwmac_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

void dwmac_platform_iface_init(struct net_if *iface)
{
	const struct device *eth_dev = net_if_get_device(iface);
	struct dwmac_priv *p = eth_dev->data;

	net_if_carrier_off(iface);

#if S4_DWMAC_HAS_PHY
	if (device_is_ready(S4_DWMAC_PHY_DEV)) {
		phy_link_callback_set(S4_DWMAC_PHY_DEV,
				      s4_dwmac_phy_link_changed,
				      (void *)eth_dev);
		printk("s4 ETH: PHY %s link callback registered\n",
		       S4_DWMAC_PHY_DEV->name);
	} else {
		printk("s4 ETH: PHY %s not ready\n",
		       S4_DWMAC_PHY_DEV->name);
	}
#else
	ARG_UNUSED(p);
#endif
}

/* S4-specific platform probe: enable clocks, MDIO mux, internal PHY */
int dwmac_platform_probe(const struct device *dev)
{
	s4_dwmac_clock_enable();
	s4_dwmac_mdio_mux_enable();

	printk("s4 ETH: platform probe done (DW MAC 3.70a, RMII internal PHY)\n");

	return 0;
}

static struct dwmac_priv dwmac_instance;

ETH_NET_DEVICE_DT_INST_DEFINE(0,
			      dwmac_probe,
			      NULL,
			      &dwmac_instance,
			      NULL,
			      CONFIG_ETH_INIT_PRIORITY,
			      &dwmac_api,
			      NET_ETH_MTU);