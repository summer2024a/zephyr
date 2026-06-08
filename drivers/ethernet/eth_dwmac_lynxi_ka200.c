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
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/phy.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/sdhc/lynxi_sysctl.h>

#include "eth_dwmac_priv.h"

#define LYNXI_GMAC_DMA_MODE_OFF 0x1000U

static inline uint32_t lynxi_lo32(uintptr_t val)
{
	return (uint32_t)val;
}

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
	printk("he200 GMAC: MAC_CONF=0x%08x CPR speed %s\n", conf,
	       PHY_LINK_IS_SPEED_1000M(speed) ? "1000M" :
	       (PHY_LINK_IS_SPEED_100M(speed) ? "100M" : "10M"));
}

#if LYNXI_DWMAC_HAS_PHY
#define LYNXI_DWMAC_MDIO_DEV DEVICE_DT_GET(DT_NODELABEL(mdio))
#define LYNXI_DWMAC_PHY_ADDR DT_REG_ADDR(DT_NODELABEL(phy0))

static void lynxi_dwmac_dma_link_up_refresh(struct dwmac_priv *p);

static void lynxi_dwmac_phy_link_changed(const struct device *phy_dev,
					 struct phy_link_state *state,
					 void *user_data)
{
	const struct device *eth_dev = user_data;
	struct dwmac_priv *p = eth_dev->data;

	ARG_UNUSED(phy_dev);

	printk("he200 GMAC: PHY link cb up=%d speed=0x%x\n",
	       state->is_up, state->speed);

	if (state->is_up) {
		lynxi_dwmac_apply_link_speed(p, state->speed);
		lynxi_dwmac_dma_link_up_refresh(p);
		net_eth_carrier_on(p->iface);
		printk("he200 GMAC: carrier ON MAC speed applied\n");
	} else {
		net_eth_carrier_off(p->iface);
		printk("he200 GMAC: carrier OFF\n");
	}
}

/*
 * phy_mii 在自协商未完成时 speed=0 且 get_link_state 强制 is_up=false，
 * 会导致 carrier 一直 off、ARP 无应答。轮询后 BMSR 仍 link 则强制 1000M。
 */
static void lynxi_dwmac_phy_sync_link(struct dwmac_priv *p, struct net_if *iface)
{
	struct phy_link_state state;

	for (int i = 0; i < 32; i++) {
		if (phy_get_link_state(LYNXI_DWMAC_PHY_DEV, &state) == 0 &&
		    state.is_up && state.speed != 0) {
			printk("he200 GMAC: PHY sync OK (iter=%d speed=0x%x)\n",
			       i, state.speed);
			return;
		}
		k_msleep(250);
	}

	if (device_is_ready(LYNXI_DWMAC_MDIO_DEV)) {
		uint16_t bmsr = 0;

		if (mdio_read(LYNXI_DWMAC_MDIO_DEV, LYNXI_DWMAC_PHY_ADDR,
			      MII_BMSR, &bmsr) == 0 &&
		    (bmsr & MII_BMSR_LINK_STATUS) != 0U) {
			printk("he200 GMAC: BMSR=0x%04x link up, force 1000M+carrier\n",
			       bmsr);
			lynxi_dwmac_apply_link_speed(p, LINK_FULL_1000BASE);
			net_eth_carrier_on(iface);
			return;
		}
		printk("he200 GMAC: PHY sync fail BMSR=0x%04x\n", bmsr);
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

/* 对齐 RT lynxi_dwmac4_mtl_init / Linux dwmac4_dma.c */
static void lynxi_dwmac_mtl_init(struct dwmac_priv *p)
{
	uint32_t rxq0;
	uint32_t tx_op;
	uint32_t rx_op;

	rxq0 = REG_READ(MAC_RXQ_CTRL0);
	rxq0 = (rxq0 & ~0x3U) | BIT(1); /* RXQ0 DCB enabled */
	REG_WRITE(MAC_RXQ_CTRL0, rxq0);
	REG_WRITE(MTL_RXQ_DMA_MAP0, 0U);

	tx_op = REG_READ(MTL_TXQn_OPERATION_MODE(0));
	REG_WRITE(MTL_TXQn_OPERATION_MODE(0),
		  tx_op | BIT(1) | BIT(3)); /* TSF + TXQEN */

	rx_op = REG_READ(MTL_RXQn_OPERATION_MODE(0));
	REG_WRITE(MTL_RXQn_OPERATION_MODE(0), rx_op | BIT(5)); /* RSF */

	printk("he200 GMAC: MTL TSF/RSF/TXQEN (rxq0=0x%08x tx=0x%08x rx=0x%08x)\n",
	       REG_READ(MAC_RXQ_CTRL0),
	       REG_READ(MTL_TXQn_OPERATION_MODE(0)),
	       REG_READ(MTL_RXQn_OPERATION_MODE(0)));
}

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

	lynxi_dwmac_mtl_init(p);
	REG_WRITE(MAC_PKT_FILTER, 0U);

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

#define LYNXI_GIC_DIST_BASE    0x08000000UL
#define LYNXI_GICD_ICFGR_OFF   0x0c00U
/* GIC-400: 0b11 = level-sensitive, active-high（Linux IRQ_TYPE_LEVEL_HIGH） */
#define LYNXI_GIC_ICFGR_LEVEL_ACTIVE_HIGH 3U

static void lynxi_gmac_irq_level_high(unsigned int intid)
{
	unsigned int idx = intid / 16U;
	unsigned int shift = (intid % 16U) * 2U;
	mem_addr_t icfgr = LYNXI_GIC_DIST_BASE + LYNXI_GICD_ICFGR_OFF + idx * 4U;
	uint32_t val = sys_read32(icfgr);

	val &= ~(BIT_MASK(2) << shift);
	val |= (LYNXI_GIC_ICFGR_LEVEL_ACTIVE_HIGH << shift);
	sys_write32(val, icfgr);
	printk("he200 GMAC: GIC ICFGR intid=%u -> level-high (reg=0x%08x)\n",
	       intid, val);
}

static void lynxi_dwmac_dma_link_up_refresh(struct dwmac_priv *p)
{
	uint32_t rx;
	unsigned int tail;

	rx = REG_READ(DMA_CHn_RX_CTRL(0));
	REG_WRITE(DMA_CHn_RX_CTRL(0), rx & ~DMA_CHn_RX_CTRL_SR);
	REG_WRITE(DMA_CHn_STATUS(0), REG_READ(DMA_CHn_STATUS(0)));

	tail = p->rx_desc_head;
	if (tail == 0U) {
		tail = NB_RX_DESCS - 1U;
	} else {
		tail--;
	}
	REG_WRITE(DMA_CHn_RXDESC_TAIL_PTR(0),
		  lynxi_lo32(p->rx_descs_phys + tail * sizeof(struct dwmac_dma_desc)));

	rx = REG_READ(DMA_CHn_RX_CTRL(0));
	REG_WRITE(DMA_CHn_RX_CTRL(0), rx | DMA_CHn_RX_CTRL_SR);

	printk("he200 GMAC: DMA RX restart tail=%u CH_STATUS=0x%08x\n",
	       tail, REG_READ(DMA_CHn_STATUS(0)));
}

#if LYNXI_DWMAC_HAS_PHY
static struct k_timer lynxi_dwmac_service_timer;

static void lynxi_dwmac_service_timer_fn(struct k_timer *timer)
{
	struct dwmac_priv *p = timer->user_data;

	ARG_UNUSED(timer);
	if (p != NULL && p->iface != NULL) {
		dwmac_service(p);
	}
}
#endif

void dwmac_platform_irq_enable(const struct device *dev)
{
	ARG_UNUSED(dev);

	lynxi_gmac_irq_level_high(DT_INST_IRQN(0));

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
		lynxi_dwmac_phy_sync_link(p, iface);
		k_timer_init(&lynxi_dwmac_service_timer, lynxi_dwmac_service_timer_fn, NULL);
		k_timer_user_data_set(&lynxi_dwmac_service_timer, p);
		k_timer_start(&lynxi_dwmac_service_timer, K_MSEC(10), K_MSEC(10));
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
