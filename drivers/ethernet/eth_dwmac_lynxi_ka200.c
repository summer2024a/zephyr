/*
 * Lynxi KA200 / HE200 platform glue for Synopsys DesignWare MAC
 *
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME lynxi_dwmac
#define LOG_LEVEL CONFIG_LYNXI_DWMAC_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define DT_DRV_COMPAT snps_designware_ethernet

#include <zephyr/kernel/mm.h>
#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/phy.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sdhc/lynxi_sysctl.h>

#include "eth_dwmac_priv.h"

#define LYNXI_GMAC_DMA_MODE_OFF 0x1000U

#if DT_NODE_HAS_STATUS(DT_INST_PHANDLE(0, phy_handle), okay)
#define LYNXI_DWMAC_HAS_PHY 1
#define LYNXI_DWMAC_PHY_DEV DEVICE_DT_GET(DT_INST_PHANDLE(0, phy_handle))
#else
#define LYNXI_DWMAC_HAS_PHY 0
#endif

static void lynxi_dwmac_apply_link_speed(struct dwmac_priv *p,
					 enum phy_link_speed speed)
{
	uint32_t conf = REG_READ(MAC_CONF);

	conf &= ~(MAC_CONF_PS | MAC_CONF_FES | MAC_CONF_DM);

	if (PHY_LINK_IS_FULL_DUPLEX(speed)) {
		conf |= MAC_CONF_DM;
	}

	if (PHY_LINK_IS_SPEED_1000M(speed)) {
		lynxi_sysctl_gmac_cpr_speed_set(1000);
	} else if (PHY_LINK_IS_SPEED_100M(speed)) {
		conf |= MAC_CONF_PS | MAC_CONF_FES;
		lynxi_sysctl_gmac_cpr_speed_set(100);
	} else {
		conf |= MAC_CONF_PS;
		lynxi_sysctl_gmac_cpr_speed_set(10);
	}

	REG_WRITE(MAC_CONF, conf);
	LOG_INF("MAC link speed updated (speed=%d)", speed);
}

#if LYNXI_DWMAC_HAS_PHY
static void lynxi_dwmac_phy_link_changed(const struct device *phy_dev,
					 struct phy_link_state *state,
					 void *user_data)
{
	const struct device *eth_dev = user_data;
	struct dwmac_priv *p = eth_dev->data;

	ARG_UNUSED(phy_dev);

	if (state->is_up) {
		lynxi_dwmac_apply_link_speed(p, state->speed);
		net_eth_carrier_on(p->iface);
	} else {
		net_eth_carrier_off(p->iface);
	}
}
#endif

int dwmac_bus_init(struct dwmac_priv *p)
{
	uintptr_t base = DT_INST_REG_ADDR(0);
	uint32_t dma_mode;

	printk("he200 GMAC: bus init start 0x8c=0x%08x\n",
	       lynxi_sysctl_cpr_read(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF));
	/*
	 * Linux dwc_qos_probe()：enable aclk → phy_ref_clk；PHY 硬复位在
	 * mdiobus_register_gpiod()（phy_mii init），不在 bus_init。
	 */
	printk("he200 GMAC: fabric+GMAC_R pulse+aclk/phy_ref/hclk\n");
	lynxi_sysctl_gmac_probe_clocks();
	k_busy_wait(500);
	printk("he200 GMAC: 0x8c=0x%08x ready\n",
	       lynxi_sysctl_cpr_read(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF));

	p->base_addr = base;

	printk("he200 GMAC: read MAC[0] @0x%lx\n", (unsigned long)base);
	dma_mode = sys_read32(base);
	printk("he200 GMAC: MAC[0]=0x%08x\n", dma_mode);

	printk("he200 GMAC: read DMA_MODE @0x%lx\n",
	       (unsigned long)(base + LYNXI_GMAC_DMA_MODE_OFF));
	dma_mode = sys_read32(base + LYNXI_GMAC_DMA_MODE_OFF);
	printk("he200 GMAC: DMA_MODE=0x%08x\n", dma_mode);

	printk("he200 GMAC: bus init OK base=0x%lx\n", (unsigned long)base);
	return 0;
}

#if (CONFIG_DCACHE_LINE_SIZE + 0 == 0)
#error "CONFIG_DCACHE_LINE_SIZE must be configured to a non-zero value"
#endif

static struct dwmac_dma_desc __aligned(CONFIG_DCACHE_LINE_SIZE)
	dwmac_tx_rx_descriptors[NB_TX_DESCS + NB_RX_DESCS];

static struct net_eth_mac_config mac_cfg = NET_ETH_MAC_DT_INST_CONFIG_INIT(0);

int dwmac_platform_init(struct dwmac_priv *p)
{
	int ret;
	uint8_t *desc_uncached_addr;
	uintptr_t desc_phys_addr;

	sys_cache_data_invd_range(dwmac_tx_rx_descriptors,
				  sizeof(dwmac_tx_rx_descriptors));

	desc_phys_addr = k_mem_phys_addr(dwmac_tx_rx_descriptors);

	k_mem_map_phys_bare(&desc_uncached_addr, desc_phys_addr,
			    sizeof(dwmac_tx_rx_descriptors),
			    K_MEM_PERM_RW | K_MEM_CACHE_NONE);

	p->tx_descs = (void *)desc_uncached_addr;
	desc_uncached_addr += NB_TX_DESCS * sizeof(struct dwmac_dma_desc);
	p->rx_descs = (void *)desc_uncached_addr;

	p->tx_descs_phys = desc_phys_addr;
	desc_phys_addr += NB_TX_DESCS * sizeof(struct dwmac_dma_desc);
	p->rx_descs_phys = desc_phys_addr;

	REG_WRITE(MAC_CONF, MAC_CONF_DM);
	REG_WRITE(DMA_SYSBUS_MODE,
		  DMA_SYSBUS_MODE_AAL |
#ifdef CONFIG_64BIT
		  DMA_SYSBUS_MODE_EAME |
#endif
		  DMA_SYSBUS_MODE_FB);

	/*
	 * Keep MAC/DMA IRQ masked until iface init; 明确屏蔽 RGMII 线中断，
	 * 避免 LEVEL IRQ 风暴（RT-Thread GmacRgmiiIntMask 同源问题）。
	 */
	REG_WRITE(MAC_IRQ_ENABLE, 0);
	REG_WRITE(MAC_IRQ_STATUS, MAC_IRQ_STATUS_RGSMIIIS);
	REG_WRITE(DMA_CHn_IRQ_ENABLE(0), 0);

	ret = net_eth_mac_load(&mac_cfg, p->mac_addr);
	if (ret == -ENODATA) {
		LOG_INF("no local-mac-address configured");
		return 0;
	}
	if (ret < 0) {
		LOG_ERR("failed to load MAC address (%d)", ret);
		return ret;
	}

	printk("he200 GMAC: platform init OK (CPR 0x8c=0x%03x)\n",
	       LYNXI_SYSCTL_GMAC_CTRL_1000M);

	return 0;
}

void dwmac_platform_irq_enable(const struct device *dev)
{
	ARG_UNUSED(dev);

	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), dwmac_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

void dwmac_platform_iface_init(struct net_if *iface)
{
	const struct device *eth_dev = net_if_get_device(iface);
	struct dwmac_priv *p = eth_dev->data;

	net_if_carrier_off(iface);

#if LYNXI_DWMAC_HAS_PHY
	if (device_is_ready(LYNXI_DWMAC_PHY_DEV)) {
		phy_link_callback_set(LYNXI_DWMAC_PHY_DEV,
				      lynxi_dwmac_phy_link_changed,
				      (void *)eth_dev);
		printk("he200 GMAC: PHY %s link callback registered\n",
		       LYNXI_DWMAC_PHY_DEV->name);
	} else {
		printk("he200 GMAC: PHY %s not ready (check mdio/gpio reset)\n",
		       LYNXI_DWMAC_PHY_DEV->name);
	}
#else
	ARG_UNUSED(p);
#endif
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
