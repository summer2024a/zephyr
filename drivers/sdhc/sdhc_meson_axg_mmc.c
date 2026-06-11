/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Amlogic Meson AXG MMC SDHC driver for S4 (S905Y4/S905W2).
 *
 * Based on Linux drivers/mmc/host/meson-gx-mmc.c and meson-gx-mmc.h.
 * The Meson MMC controller has a custom register layout different from
 * DesignWare DWC MSHC or standard SDHCI.
 *
 * Key registers (from meson-gx-mmc.h):
 *   ARGU       +0x00  — Command argument
 *   SEND       +0x04  — Command / transfer config
 *   DATA       +0x08  — Data buffer (up to 512 bytes for PIO)
 *   STATUS     +0x0C  — Status register
 *   CTRL       +0x10  — Clock / bus width config
 *   CLOCK      +0x14  — Clock divider
 *   RESP0      +0x18  — Response register 0
 *   RESP1      +0x1C  — Response register 1
 *   RESP2      +0x20  — Response register 2
 *   RESP3      +0x24  — Response register 3
 *   DMA_ADDR   +0x28  — DMA descriptor address
 *   BLK_CNTL   +0x2C  — Block count and size
 *   CFG        +0x40  — DMA config
 *   ADMA_DESC  +0x48  — ADMA descriptor list address (S4)
 *   PINMUX     +0x50  — Pin mux config
 *   PWR        +0x58  — Power control
 *
 * This is a skeleton driver providing basic MMC/eMMC/SD functionality.
 * DMA support and high-speed modes require further implementation.
 */

#define DT_DRV_COMPAT amlogic_meson_axg_mmc

#include <zephyr/drivers/sdhc.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/mmc/mmc.h>

LOG_MODULE_REGISTER(sdhc_meson, CONFIG_SDHC_LOG_LEVEL);

/* Register offsets (from Linux meson-gx-mmc.h) */
#define MESON_MMC_ARGU        0x00U
#define MESON_MMC_SEND        0x04U
#define MESON_MMC_DATA        0x08U
#define MESON_MMC_STATUS      0x0CU
#define MESON_MMC_CTRL        0x10U
#define MESON_MMC_CLOCK       0x14U
#define MESON_MMC_RESP0       0x18U
#define MESON_MMC_RESP1       0x1CU
#define MESON_MMC_RESP2       0x20U
#define MESON_MMC_RESP3       0x24U
#define MESON_MMC_DMA_ADDR    0x28U
#define MESON_MMC_BLK_CNTL    0x2CU
#define MESON_MMC_CFG         0x40U
#define MESON_MMC_ADMA_DESC   0x48U
#define MESON_MMC_PINMUX      0x50U
#define MESON_MMC_PWR         0x58U

/* SEND register bits */
#define SEND_CMD_INDEX_MASK   0x3FU
#define SEND_CMD_INDEX_SHIFT  0U
#define SEND_RESP_CMD7_NO_CRC (1U << 7)
#define SEND_RESP_NO_DATA     (1U << 8)
#define SEND_RESP_R1          (1U << 9)
#define SEND_RESP_R2          (2U << 9)
#define SEND_RESP_R3          (3U << 9)
#define SEND_RESP_R4          (4U << 9)
#define SEND_DATA_STOP        (1U << 14)
#define SEND_DATA_WRITE       (1U << 15)
#define SEND_DATA_READ        (1U << 16)
#define SEND_RESP_LEN_MASK    (1U << 17)
#define SEND_RESP_LEN_136     (0U << 17)
#define SEND_RESP_LEN_48      (1U << 17)
#define SEND_CHK_RESP_CRC     (1U << 19)
#define SEND_CMD_HAS_DATA     (1U << 20)
#define SEND_CMD_PACK_LEN_MASK (0x3FU << 24)

/* STATUS register bits */
#define STATUS_RX_BUSY        (1U << 0)
#define STATUS_TX_BUSY        (1U << 1)
#define STATUS_RESP_CRC_OK    (1U << 2)
#define STATUS_DATA_CRC_OK    (1U << 7)
#define STATUS_CMD_OK         (1U << 8)
#define STATUS_FIFO_EMPTY     (1U << 11)
#define STATUS_FIFO_FULL      (1U << 12)

/* CTRL register bits */
#define CTRL_CMD_PERIOD_MASK  0xFU
#define CTRL_DDR_MODE         (1U << 8)
#define CTRL_TX_ENDIAN_MASK   (0x7U << 9)
#define CTRL_RX_ENDIAN_MASK   (0x7U << 12)
#define CTRL_BUS_WIDTH_MASK   (0x3U << 16)
#define CTRL_BUS_WIDTH_1      (0U << 16)
#define CTRL_BUS_WIDTH_4      (1U << 16)
#define CTRL_BUS_WIDTH_8      (2U << 16)
#define CTRL_PACK_LEN_MASK    (0xFU << 24)
#define CTRL_MANUAL_STOP      (1U << 28)
#define CTRL_AUTO_STOP        (1U << 29)

/* CLOCK register bits */
#define CLOCK_SRC_MASK        (0x3U << 0)
#define CLOCK_SRC_24M         (0U << 0)
#define CLOCK_DIV_MASK        (0x3FU << 4)
#define CLOCK_MAX_DIV         63U

/* CFG register bits */
#define CFG_BLK_SIZE_MASK     (0xFU << 0)
#define CFG_RESP_ERR_MASK     (0x3U << 8)
#define CFG_RESP_TIMEOUT_MASK (0xFU << 12)
#define CFG_DATA_TIMEOUT_MASK (0xFU << 16)
#define CFG_IRQ_EN            (1U << 24)
#define CFG_DMA_EN            (1U << 28)
#define CFG_DMA_DESC_MODE     (1U << 29)

struct meson_mmc_config {
	uint32_t base;
	uint32_t irq;
	uint32_t clock_freq;  /* Input clock (XTAL 24MHz or peripheral clock) */
	uint32_t max_bus_freq;
	uint32_t min_bus_freq;
};

struct meson_mmc_data {
	uint32_t current_clock;
	uint32_t current_bus_width;
};

static inline uint32_t mmc_read(const struct device *dev, uint32_t offset)
{
	const struct meson_mmc_config *cfg = dev->config;

	return sys_read32(cfg->base + offset);
}

static inline void mmc_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	const struct meson_mmc_config *cfg = dev->config;

	sys_write32(value, cfg->base + offset);
}

static int meson_mmc_reset(const struct device *dev)
{
	/* Soft reset: clear all control bits, set defaults */
	mmc_write(dev, MESON_MMC_CTRL, 0);
	mmc_write(dev, MESON_MMC_SEND, 0);
	mmc_write(dev, MESON_MMC_CLOCK, CLOCK_SRC_24M);
	mmc_write(dev, MESON_MMC_CFG, 0);

	/* Wait for busy bits to clear */
	uint32_t status;
	int timeout = 1000;

	do {
		status = mmc_read(dev, MESON_MMC_STATUS);
		if (!(status & (STATUS_RX_BUSY | STATUS_TX_BUSY))) {
			break;
		}
		k_busy_wait(100);
		timeout--;
	} while (timeout > 0);

	if (timeout <= 0) {
		LOG_ERR("MMC reset timeout, status=0x%x", status);
		return -ETIMEDOUT;
	}

	return 0;
}

static int meson_mmc_set_clock(const struct device *dev, uint32_t clock_freq)
{
	const struct meson_mmc_config *cfg = dev->config;
	struct meson_mmc_data *data = dev->data;
	uint32_t clk_reg;
	uint32_t div;

	if (clock_freq == 0) {
		/* Disable clock */
		mmc_write(dev, MESON_MMC_CLOCK, 0);
		data->current_clock = 0;
		return 0;
	}

	/* Calculate clock divider from XTAL (24MHz) */
	if (clock_freq >= cfg->clock_freq) {
		div = 0; /* No division — use source clock directly */
	} else {
		div = (cfg->clock_freq / clock_freq);
		if (div > CLOCK_MAX_DIV) {
			div = CLOCK_MAX_DIV;
		}
	}

	clk_reg = CLOCK_SRC_24M | (div << 4);
	mmc_write(dev, MESON_MMC_CLOCK, clk_reg);
	data->current_clock = cfg->clock_freq / (div + 1);

	LOG_DBG("MMC clock set: target=%u, div=%u, actual=%u",
		clock_freq, div, data->current_clock);

	return 0;
}

static int meson_mmc_set_bus_width(const struct device *dev, enum sdhc_bus_width width)
{
	struct meson_mmc_data *data = dev->data;
	uint32_t ctrl;
	uint32_t bus_width_val;

	switch (width) {
	case SDHC_BUS_WIDTH_1BIT:
		bus_width_val = CTRL_BUS_WIDTH_1;
		data->current_bus_width = 1;
		break;
	case SDHC_BUS_WIDTH_4BIT:
		bus_width_val = CTRL_BUS_WIDTH_4;
		data->current_bus_width = 4;
		break;
	case SDHC_BUS_WIDTH_8BIT:
		bus_width_val = CTRL_BUS_WIDTH_8;
		data->current_bus_width = 8;
		break;
	default:
		return -ENOTSUP;
	}

	ctrl = mmc_read(dev, MESON_MMC_CTRL);
	ctrl &= ~CTRL_BUS_WIDTH_MASK;
	ctrl |= bus_width_val;
	mmc_write(dev, MESON_MMC_CTRL, ctrl);

	return 0;
}

static int meson_mmc_request(const struct device *dev, struct sdhc_cmd *cmd,
			     struct sdhc_data *data)
{
	uint32_t send_cmd = 0;
	uint32_t status;
	int timeout = 50000; /* 5 seconds max */
	int ret = 0;

	/* Prepare command */
	send_cmd = (cmd->opcode & SEND_CMD_INDEX_MASK) << SEND_CMD_INDEX_SHIFT;

	switch (cmd->response_type) {
	case SD_RSP_TYPE_NONE:
		send_cmd |= SEND_RESP_NO_DATA;
		break;
	case SD_RSP_TYPE_R1:
		send_cmd |= SEND_RESP_LEN_48 | SEND_RESP_R1 | SEND_CHK_RESP_CRC;
		break;
	case SD_RSP_TYPE_R1b:
		send_cmd |= SEND_RESP_LEN_48 | SEND_RESP_R1 | SEND_CHK_RESP_CRC;
		break;
	case SD_RSP_TYPE_R2:
		send_cmd |= SEND_RESP_LEN_136 | SEND_RESP_R2 | SEND_CHK_RESP_CRC;
		break;
	case SD_RSP_TYPE_R3:
		send_cmd |= SEND_RESP_LEN_48 | SEND_RESP_R3;
		break;
	case SD_RSP_TYPE_R4:
		send_cmd |= SEND_RESP_LEN_48 | SEND_RESP_R4;
		break;
	default:
		LOG_ERR("Unsupported response type %d", cmd->response_type);
		return -ENOTSUP;
	}

	/* Data transfer */
	if (data) {
		send_cmd |= SEND_CMD_HAS_DATA;
		if (data->data_write) {
			send_cmd |= SEND_DATA_WRITE;
		} else {
			send_cmd |= SEND_DATA_READ;
		}

		/* Set block count and size */
		uint32_t blk_cntl = ((data->blocks << 16) & 0xFFFF0000) |
				    (data->block_size & 0xFFFF);
		mmc_write(dev, MESON_MMC_BLK_CNTL, blk_cntl);
	}

	/* Write command argument */
	mmc_write(dev, MESON_MMC_ARGU, cmd->arg);

	/* Send command */
	mmc_write(dev, MESON_MMC_SEND, send_cmd);

	/* Wait for command completion */
	do {
		status = mmc_read(dev, MESON_MMC_STATUS);
		if (status & STATUS_CMD_OK) {
			break;
		}
		k_busy_wait(100);
		timeout--;
	} while (timeout > 0);

	if (timeout <= 0) {
		LOG_ERR("MMC command timeout, opcode=%d, status=0x%x",
			cmd->opcode, status);
		cmd->error = -ETIMEDOUT;
		return -ETIMEDOUT;
	}

	/* Read response */
	cmd->response[0] = mmc_read(dev, MESON_MMC_RESP0);
	if (cmd->response_type == SD_RSP_TYPE_R2) {
		cmd->response[1] = mmc_read(dev, MESON_MMC_RESP1);
		cmd->response[2] = mmc_read(dev, MESON_MMC_RESP2);
		cmd->response[3] = mmc_read(dev, MESON_MMC_RESP3);
	}

	/* Check CRC */
	if ((send_cmd & SEND_CHK_RESP_CRC) && !(status & STATUS_RESP_CRC_OK)) {
		LOG_ERR("MMC CRC error, status=0x%x", status);
		cmd->error = -EILSEQ;
		return -EILSEQ;
	}

	LOG_DBG("MMC cmd %d OK, resp0=0x%08x", cmd->opcode, cmd->response[0]);

	return ret;
}

static int meson_mmc_card_busy(const struct device *dev)
{
	uint32_t status = mmc_read(dev, MESON_MMC_STATUS);

	return (status & (STATUS_RX_BUSY | STATUS_TX_BUSY)) ? 1 : 0;
}

static int meson_mmc_get_host_props(const struct device *dev,
				    struct sdhc_host_props *props)
{
	const struct meson_mmc_config *cfg = dev->config;

	props->f_max = cfg->max_bus_freq;
	props->f_min = cfg->min_bus_freq;
	props->max_bus_width = SDHC_BUS_WIDTH_8BIT;
	props->host_caps.high_spd_support = 1;
	props->host_caps.sdr104_support = 0;
	props->host_caps.ddr50_support = 0;
	props->host_caps.bus_8_bit_support = 1;
	props->host_caps.bus_4_bit_support = 1;
	props->host_caps.vol_330_support = 1;
	props->host_caps.vol_180_support = 1;

	return 0;
}

static int meson_mmc_init(const struct device *dev)
{
	int ret;

	ret = meson_mmc_reset(dev);
	if (ret) {
		return ret;
	}

	/* Set default clock (identification mode: 400kHz max) */
	ret = meson_mmc_set_clock(dev, 400000);
	if (ret) {
		return ret;
	}

	/* Set default bus width (1-bit for identification) */
	meson_mmc_set_bus_width(dev, SDHC_BUS_WIDTH_1BIT);

	/* Enable interrupts in CFG register */
	mmc_write(dev, MESON_MMC_CFG,
		  CFG_IRQ_EN |
		  (15U << 12) | /* resp_timeout */
		  (15U << 16)); /* data_timeout */

	LOG_INF("Meson MMC initialized at 0x%x", ((struct meson_mmc_config *)dev->config)->base);

	return 0;
}

static const struct sdhc_driver_api meson_mmc_driver_api = {
	.reset = meson_mmc_reset,
	.request = meson_mmc_request,
	.set_bus_width = meson_mmc_set_bus_width,
	.set_clock = meson_mmc_set_clock,
	.card_busy = meson_mmc_card_busy,
	.get_host_props = meson_mmc_get_host_props,
};

#define MESON_MMC_INIT(inst)                                            \
	static struct meson_mmc_data meson_mmc_data_##inst;             \
	static const struct meson_mmc_config meson_mmc_config_##inst = { \
		.base = DT_INST_REG_ADDR(inst),                             \
		.irq = DT_INST_IRQN(inst),                                  \
		.clock_freq = DT_INST_PROP(inst, clock_frequency),          \
		.max_bus_freq = DT_INST_PROP(inst, max_bus_freq),           \
		.min_bus_freq = DT_INST_PROP(inst, min_bus_freq),           \
	};                                                              \
	DEVICE_DT_INST_DEFINE(inst,                                     \
			      meson_mmc_init,                              \
			      NULL,                                         \
			      &meson_mmc_data_##inst,                        \
			      &meson_mmc_config_##inst,                      \
			      POST_KERNEL,                                  \
			      CONFIG_SDHC_INIT_PRIORITY,                     \
			      &meson_mmc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MESON_MMC_INIT)