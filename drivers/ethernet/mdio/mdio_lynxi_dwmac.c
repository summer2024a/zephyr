/*
 * Lynxi KA200 DWMAC4 embedded MDIO
 *
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clause-22 MDIO via GMAC_MDIO_ADDR/DATA @ offset 0x200 from MAC base.
 * Aligned with Linux stmmac_mdio.c (GMAC4 read/write).
 */

#define DT_DRV_COMPAT lynxi_dwmac_mdio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sdhc/lynxi_sysctl.h>
#include <zephyr/net/mii.h>

LOG_MODULE_REGISTER(mdio_lynxi_dwmac, CONFIG_MDIO_LOG_LEVEL);

/* lynchip-lite-evb.dts ethernet-phy@1 */
#define LYNXI_EVB_PHY_ADDR  1U

#define MAC_MDIO_ADDRESS 0x0200U
#define MAC_MDIO_DATA    0x0204U

#define MII_BUSY         BIT(0)
#define MII_GMAC4_READ   (3U << 2)
#define MII_GMAC4_WRITE  (1U << 2)

#define MII_ADDR_PA_SHIFT  21U
#define MII_ADDR_RDA_SHIFT 16U
#define MII_ADDR_CR_SHIFT  8U
#define MII_ADDR_PA_MASK   GENMASK(25, 21)
#define MII_ADDR_RDA_MASK  GENMASK(20, 16)
#define MII_ADDR_CR_MASK   GENMASK(11, 8)
#define MII_DATA_MASK      GENMASK(15, 0)

struct mdio_lynxi_dwmac_config {
	uintptr_t mac_base;
	uint8_t csr_clock_range;
};

struct mdio_lynxi_dwmac_data {
	struct k_mutex lock;
};

static int mdio_wait_idle(uintptr_t mac_base, k_timepoint_t timeout)
{
	uint32_t v;

	while (1) {
		v = sys_read32(mac_base + MAC_MDIO_ADDRESS);
		if ((v & MII_BUSY) == 0U) {
			return 0;
		}
		if (sys_timepoint_expired(timeout)) {
			return -ETIMEDOUT;
		}
		k_busy_wait(10);
	}
}

static int mdio_transfer(const struct device *dev, uint8_t prtad, uint8_t regad,
			 bool write, uint16_t data_in, uint16_t *data_out)
{
	const struct mdio_lynxi_dwmac_config *cfg = dev->config;
	struct mdio_lynxi_dwmac_data *data = dev->data;
	uintptr_t mac_base = cfg->mac_base;
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(100));
	uint32_t addr_val;
	int ret;

	ret = mdio_wait_idle(mac_base, timeout);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (write) {
		sys_write32(data_in & MII_DATA_MASK, mac_base + MAC_MDIO_DATA);
	}

	addr_val = MII_BUSY;
	addr_val |= FIELD_PREP(MII_ADDR_PA_MASK, prtad);
	addr_val |= FIELD_PREP(MII_ADDR_RDA_MASK, regad);
	addr_val |= FIELD_PREP(MII_ADDR_CR_MASK, cfg->csr_clock_range);
	addr_val |= write ? MII_GMAC4_WRITE : MII_GMAC4_READ;

	sys_write32(addr_val, mac_base + MAC_MDIO_ADDRESS);

	ret = mdio_wait_idle(mac_base, timeout);
	if (ret == 0 && !write && data_out != NULL) {
		*data_out = sys_read32(mac_base + MAC_MDIO_DATA) & MII_DATA_MASK;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int mdio_lynxi_dwmac_read(const struct device *dev, uint8_t prtad,
				 uint8_t regad, uint16_t *data)
{
	return mdio_transfer(dev, prtad, regad, false, 0, data);
}

static int mdio_lynxi_dwmac_write(const struct device *dev, uint8_t prtad,
				  uint8_t regad, uint16_t data)
{
	return mdio_transfer(dev, prtad, regad, true, data, NULL);
}

static int mdio_lynxi_dwmac_init(const struct device *dev)
{
	const struct mdio_lynxi_dwmac_config *cfg = dev->config;
	struct mdio_lynxi_dwmac_data *data = dev->data;

	/* PHY 硬复位在 phy_mii init（Linux mdiobus_register_gpiod 等价） */

	k_mutex_init(&data->lock);
	printk("he200 GMAC: mdio %s init (MAC 0x%lx csr_cr=%u)\n", dev->name,
	       (unsigned long)cfg->mac_base, cfg->csr_clock_range);

	{
		uint16_t phyid1 = 0U;
		int ret = mdio_lynxi_dwmac_read(dev, LYNXI_EVB_PHY_ADDR,
						MII_PHYID1R, &phyid1);

		printk("he200 GMAC: mdio PHYID1 probe addr=%u val=0x%04x ret=%d\n",
		       LYNXI_EVB_PHY_ADDR, phyid1, ret);
	}

	return 0;
}

static DEVICE_API(mdio, mdio_lynxi_dwmac_api) = {
	.read = mdio_lynxi_dwmac_read,
	.write = mdio_lynxi_dwmac_write,
};

#define MDIO_LYNXI_DWMAC_DEFINE(n)						\
	static const struct mdio_lynxi_dwmac_config mdio_lynxi_dwmac_cfg_##n = { \
		.mac_base = DT_REG_ADDR(DT_INST_PARENT(n)),			\
		.csr_clock_range = DT_INST_PROP(n, csr_clock_range),		\
	};									\
	static struct mdio_lynxi_dwmac_data mdio_lynxi_dwmac_data_##n;		\
	DEVICE_DT_INST_DEFINE(n, mdio_lynxi_dwmac_init, NULL,			\
			      &mdio_lynxi_dwmac_data_##n,			\
			      &mdio_lynxi_dwmac_cfg_##n, POST_KERNEL,		\
			      CONFIG_MDIO_INIT_PRIORITY, &mdio_lynxi_dwmac_api);

DT_INST_FOREACH_STATUS_OKAY(MDIO_LYNXI_DWMAC_DEFINE)
