/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lynxi HE200 DWC MSHC SDHCI — ported from RT-Thread bsp/lynxi/he200/drivers/drv_sdhci.c
 */

#define DT_DRV_COMPAT lynxi_dwcmshc_sdhci

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/irq.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sdhc/lynxi_sysctl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include "sdhc_lynxi_dwcmshc_regs.h"

LOG_MODULE_REGISTER(sdhc_lynxi, CONFIG_SDHC_LOG_LEVEL);

#if defined(CONFIG_LYNXI_DWCMSHC_CMD_DEBUG)
static atomic_t lynxi_cmd_seq;

static const char *lynxi_opcode_name(uint32_t opcode, uint32_t arg)
{
	switch (opcode) {
	case 0:  return "GO_IDLE_STATE";
	case 1:  return "SEND_OP_COND";
	case 8:
		if (arg == 0x1aaU) {
			return "SD_SEND_IF_COND";
		}
		return "MMC_SEND_EXT_CSD";
	case 2:  return "ALL_SEND_CID";
	case 3:  return "SET_RELATIVE_ADDR";
	case 6:  return "SWITCH";
	case 7:  return "SELECT_CARD";
	case 9:  return "SEND_CSD";
	case 12: return "STOP_TRANSMISSION";
	case 13: return "SEND_STATUS";
	case 17: return "READ_SINGLE_BLOCK";
	case 18: return "READ_MULTIPLE_BLOCK";
	case 24: return "WRITE_BLOCK";
	case 25: return "WRITE_MULTIPLE_BLOCK";
	case 55: return "APP_CMD";
	default: return "?";
	}
}

#define lynxi_cmd_printk(fmt, ...) printk("lynxi-sdhc: " fmt, ##__VA_ARGS__)
#else
#define lynxi_cmd_printk(fmt, ...)
#endif

struct lynxi_dwcmshc_config {
	uintptr_t reg_base;
	uint32_t max_clk;
	uint32_t min_clk;
	uint32_t power_delay_ms;
	bool is_emmc;
	bool io_1v8;
	bool hs200;
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
	unsigned int irq;
#endif
	void (*irq_config)(const struct device *dev);
};

struct lynxi_dwcmshc_data {
	struct k_mutex bus_mutex;
	struct sdhc_io host_io;
	struct sdhc_host_props props;
	uint8_t timing;
	bool hw_inited;
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
	struct k_sem irq_sem;
	atomic_t irq_count;
	uint32_t irq_last;
	uint32_t error_code;
	bool irq_seen;
#endif
};

#define LYNXI_REG(dev) ((mm_reg_t)((const struct lynxi_dwcmshc_config *)(dev)->config)->reg_base)

static inline uint32_t lynxi_readl(const struct device *dev, int reg)
{
	return sys_read32(LYNXI_REG(dev) + reg);
}

static inline void lynxi_writel(const struct device *dev, uint32_t val, int reg)
{
	sys_write32(val, LYNXI_REG(dev) + reg);
}

static inline uint16_t lynxi_readw(const struct device *dev, int reg)
{
	return sys_read16(LYNXI_REG(dev) + reg);
}

static inline void lynxi_writew(const struct device *dev, uint16_t val, int reg)
{
	sys_write16(val, LYNXI_REG(dev) + reg);
}

static inline uint8_t lynxi_readb(const struct device *dev, int reg)
{
	return sys_read8(LYNXI_REG(dev) + reg);
}

static inline void lynxi_writeb(const struct device *dev, uint8_t val, int reg)
{
	sys_write8(val, LYNXI_REG(dev) + reg);
}

static void lynxi_phy_1_8v(const struct device *dev)
{
	lynxi_writew(dev, 0x669, DWC_MSHC_CMDPAD_CNFG);
	lynxi_writew(dev, 0x669, DWC_MSHC_DATPAD_CNFG);
	lynxi_writew(dev, 0x660, DWC_MSHC_CLKPAD_CNFG);
	lynxi_writew(dev, 0x671, DWC_MSHC_STBPAD_CNFG);
	lynxi_writew(dev, 0x669, DWC_MSHC_RSTNPAD_CNFG);
}

static void lynxi_phy_3_3v(const struct device *dev)
{
	lynxi_writew(dev, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_CMDPAD_CNFG);
	lynxi_writew(dev, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_DATPAD_CNFG);
	lynxi_writew(dev, DWC_MSHC_PHY_PAD_EMMC_CLK, DWC_MSHC_CLKPAD_CNFG);
	lynxi_writew(dev, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_STBPAD_CNFG);
	lynxi_writew(dev, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_RSTNPAD_CNFG);
}

static int lynxi_phy_init(const struct device *dev)
{
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	uint32_t reg;
	uint32_t clk;
	int timeout = 15000;

	clk = lynxi_readw(dev, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_INT_EN;
	lynxi_writew(dev, clk, SDHCI_CLOCK_CONTROL);
	lynxi_writeb(dev, 0x10, DWC_MSHC_SDCLKDL_CNFG);
	lynxi_writeb(dev, 0x7f, DWC_MSHC_SDCLKDL_DC);
	lynxi_writeb(dev, 0x00, DWC_MSHC_SDCLKDL_CNFG);

	clk = lynxi_readw(dev, SDHCI_CLOCK_CONTROL);
	clk |= SDHCI_CLOCK_INT_EN;
	lynxi_writew(dev, clk, SDHCI_CLOCK_CONTROL);

	lynxi_writew(dev, 0, DWC_MSHC_PHY_CNFG);
	k_busy_wait(10);

	reg = PAD_SN_DEFAULT | PAD_SP_DEFAULT | PHY_PWRGOOD;
	lynxi_writel(dev, reg, DWC_MSHC_PHY_CNFG);

	if (cfg->io_1v8) {
		uint16_t h2 = lynxi_readw(dev, SDHCI_HOST_CONTROL2);

		h2 |= SDHCI_CTRL_VDD_180;
		lynxi_writew(dev, h2, SDHCI_HOST_CONTROL2);
		lynxi_phy_1_8v(dev);
	} else {
		lynxi_phy_3_3v(dev);
	}

	lynxi_writeb(dev, 0x8, DWC_MSHC_SMPLDL_CNFG);

	while (timeout-- > 0) {
		reg = lynxi_readl(dev, DWC_MSHC_PHY_CNFG);
		if (reg & PHY_PWRGOOD) {
			break;
		}
		k_busy_wait(100);
	}
	if (timeout <= 0) {
		return -EIO;
	}

	reg |= PHY_RSTN;
	lynxi_writel(dev, reg, DWC_MSHC_PHY_CNFG);
	lynxi_writeb(dev, 0x11, MSHC_CTRL_R);

	/* Linux lyn_sdhci_dwcmshc_set_phy: CARD_IS_EMMC before any card clock/CMD */
	if (cfg->is_emmc) {
		uint32_t emmc = lynxi_readl(dev, EMMC_CTRL_R);

		emmc |= BIT(CARD_IS_EMMC);
		lynxi_writel(dev, emmc, EMMC_CTRL_R);
	}

	return 0;
}

/* RT lyn_sdhci_dwcmshc_dl_cfg: tap_max + SMPLDL/ATDL/SDCLKDL before card clock */
static void lynxi_dl_cfg(const struct device *dev)
{
	struct lynxi_dwcmshc_data *data = dev->data;
	uint32_t tap_max = 0x63;
	uint16_t clk;

	switch (data->timing) {
	case SDHC_TIMING_HS200:
		tap_max = 0x2b;
		break;
	case SDHC_TIMING_HS400:
		tap_max = 0x23;
		break;
	case SDHC_TIMING_SDR104:
	case SDHC_TIMING_DDR50:
	case SDHC_TIMING_DDR52:
		tap_max = 0x60;
		break;
	default:
		tap_max = 0x63;
		break;
	}

	clk = lynxi_readw(dev, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	lynxi_writew(dev, clk, SDHCI_CLOCK_CONTROL);

	lynxi_writeb(dev, 0x08, DWC_MSHC_SMPLDL_CNFG);
	lynxi_writeb(dev, 0x08, DWC_MSHC_ATDL_CNFG);
	lynxi_writeb(dev, 0x10, DWC_MSHC_SDCLKDL_CNFG);
	lynxi_writeb(dev, tap_max, DWC_MSHC_SDCLKDL_DC);
	k_busy_wait(5);
	lynxi_writeb(dev, 0x00, DWC_MSHC_SDCLKDL_CNFG);

	clk |= SDHCI_CLOCK_CARD_EN;
	lynxi_writew(dev, clk, SDHCI_CLOCK_CONTROL);

	LOG_DBG("timing=%u tap=0x%x clk=0x%x", data->timing, tap_max,
		lynxi_readw(dev, SDHCI_CLOCK_CONTROL));
}

/* RT sdhci_set_uhs_signaling — skipped for eMMC in lx_set_iocfg */
static void lynxi_set_uhs_signaling(const struct device *dev, uint8_t timing)
{
	uint16_t ctrl2 = lynxi_readw(dev, SDHCI_HOST_CONTROL2);

	ctrl2 &= ~SDHCI_CTRL_UHS_MASK;
	switch (timing) {
	case SDHC_TIMING_HS200:
	case SDHC_TIMING_SDR104:
		ctrl2 |= SDHCI_CTRL_UHS_SDR104;
		break;
	case SDHC_TIMING_SDR12:
		ctrl2 |= SDHCI_CTRL_UHS_SDR12;
		break;
	case SDHC_TIMING_SDR25:
		ctrl2 |= SDHCI_CTRL_UHS_SDR25;
		break;
	case SDHC_TIMING_SDR50:
		ctrl2 |= SDHCI_CTRL_UHS_SDR50;
		break;
	case SDHC_TIMING_DDR50:
	case SDHC_TIMING_DDR52:
		ctrl2 |= SDHCI_CTRL_UHS_DDR50;
		break;
	case SDHC_TIMING_HS400:
		ctrl2 |= SDHCI_CTRL_HS400;
		break;
	default:
		break;
	}
	lynxi_writew(dev, ctrl2, SDHCI_HOST_CONTROL2);
}

/* RT dwcmshc_phy_delay_config AT vendor bits (function unused in RT; needed for HS200 path) */
static void lynxi_at_vendor_init(const struct device *dev)
{
	uint32_t at;

	at = lynxi_readl(dev, SDHCI_VENDER_AT_CTRL_REG);
	at |= BIT(16) | BIT(17) | BIT(19) | BIT(20);
	lynxi_writel(dev, at, SDHCI_VENDER_AT_CTRL_REG);
	lynxi_writel(dev, 0, SDHCI_VENDER_AT_STAT_REG);
}

static int lynxi_clock_set(const struct device *dev, uint32_t clock)
{
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	uint32_t div, val = 0;
	int stable_us = 200000;

	if (clock == 0) {
		return 0;
	}

	lynxi_dl_cfg(dev);

	if (cfg->max_clk <= clock) {
		div = 1;
	} else {
		for (div = 2; div < SDHCI_MAX_DIV_SPEC_300; div += 2) {
			if ((cfg->max_clk / div) <= clock) {
				break;
			}
		}
	}
	div >>= 1;
	val |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	val |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN) << 6;
	val |= SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_PLL_EN;
	lynxi_writew(dev, val, SDHCI_CLOCK_CONTROL);
	while ((lynxi_readw(dev, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) == 0) {
		if (--stable_us == 0) {
			LOG_ERR("clock internal stable timeout");
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	val |= SDHCI_CLOCK_CARD_EN;
	lynxi_writew(dev, val, SDHCI_CLOCK_CONTROL);

	/* RT lx_set_iocfg: HISPD follows card clock, not identification 400kHz */
	{
		uint8_t ctrl = lynxi_readb(dev, SDHCI_HOST_CONTROL);

		if (clock > 26000000U) {
			ctrl |= SDHCI_CTRL_HISPD;
		} else {
			ctrl &= ~SDHCI_CTRL_HISPD;
		}
		lynxi_writeb(dev, ctrl, SDHCI_HOST_CONTROL);
	}
	return 0;
}

static void lynxi_reset(const struct device *dev, uint8_t mask)
{
	int timeout = 50;

	lynxi_writeb(dev, mask, SDHCI_SOFTWARE_RESET);
	while (lynxi_readb(dev, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout-- == 0) {
			return;
		}
		k_busy_wait(100);
	}
}

static void lynxi_hw_init(const struct device *dev)
{
	struct lynxi_dwcmshc_data *data = dev->data;
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	uint16_t ctrl2;

	if (data->hw_inited) {
		return;
	}

	lynxi_sysctl_emmc_enable();
	lynxi_reset(dev, SDHCI_RESET_ALL);

	ctrl2 = lynxi_readw(dev, SDHCI_HOST_CONTROL2);
	ctrl2 |= SDHCI_CTRL_V4_MODE | SDHCI_CTRL_64BIT_ADDR | SDHCI_CMD23_ENABLE;
	ctrl2 &= ~SDHCI_CTRL_UHS_MASK;
	ctrl2 |= SDHCI_CTRL_UHS_SDR12;
	if (cfg->is_emmc) {
		ctrl2 |= SDHCI_CTRL_VDD_180; /* RT: host->is_emmc_card */
	}
	lynxi_writew(dev, ctrl2, SDHCI_HOST_CONTROL2);

	/*
	 * RT sdhci_init: HISPD here, cleared by lx_set_iocfg when clock <= 26MHz.
	 * No card clock, EMMC_CTRL, RST_N, or at_vendor until lx_set_iocfg / HS200.
	 */
	lynxi_writeb(dev, SDHCI_CTRL_HISPD, SDHCI_HOST_CONTROL);
	lynxi_writeb(dev, SDHCI_POWER_ON | SDHCI_POWER_330, SDHCI_POWER_CONTROL);
	lynxi_writel(dev, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK, SDHCI_INT_ENABLE);
	lynxi_writel(dev, 0, SDHCI_SIGNAL_ENABLE);

	(void)lynxi_phy_init(dev);

	data->timing = SDHC_TIMING_LEGACY;
	data->hw_inited = true;
}

/* RT sdhci_transfer_blocking: spin until inhibit clears (no timeout). */
static void lynxi_wait_inhibit_clear(const struct device *dev, uint32_t mask)
{
	while (lynxi_readl(dev, SDHCI_PRESENT_STATE) & mask) {
	}
}

/* SDHCI PRESENT_STATE: DAT0 low = card busy (R1b / programming) */
static int lynxi_card_busy(const struct device *dev)
{
	uint32_t ps = lynxi_readl(dev, SDHCI_PRESENT_STATE);

	if ((ps & SDHCI_CMD_INHIBIT) || (ps & SDHCI_DATA_INHIBIT)) {
		return 1;
	}
	if (!(ps & SDHCI_DAT0_SIGNAL_LEVEL)) {
		return 1;
	}
	return 0;
}

#if defined(CONFIG_LYNXI_DWCMSHC_DEBUG)
static void lynxi_dbg_dump(const struct device *dev, const char *tag)
{
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	uint32_t present = lynxi_readl(dev, SDHCI_PRESENT_STATE);
	uint16_t ver = lynxi_readw(dev, SDHCI_HOST_VERSION);
	uint32_t cap = lynxi_readl(dev, SDHCI_CAPABILITIES);
	uint32_t ist = lynxi_readl(dev, SDHCI_INT_STATUS);
	uint32_t ien = lynxi_readl(dev, SDHCI_INT_ENABLE);
	uint32_t sig = lynxi_readl(dev, SDHCI_SIGNAL_ENABLE);
	uint32_t emmc = lynxi_readl(dev, EMMC_CTRL_R);

	printk("lynxi-sdhc [%s] base=0x%lx emmc=%d\n", tag, (unsigned long)cfg->reg_base,
	       cfg->is_emmc ? 1 : 0);
	printk("  ver=0x%04x cap=0x%08x present=0x%08x emmc_ctrl=0x%08x\n", ver, cap, present,
	       emmc);
	printk("  int_st=0x%08x int_en=0x%08x sig_en=0x%08x clk=0x%04x pwr=0x%02x\n", ist, ien,
	       sig, lynxi_readw(dev, SDHCI_CLOCK_CONTROL), lynxi_readb(dev, SDHCI_POWER_CONTROL));
}
#else
#define lynxi_dbg_dump(dev, tag) ARG_UNUSED(dev); ARG_UNUSED(tag)
#endif

#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
/* RT sdhci_irq: record status, wake, clear INT_STATUS */
static void lynxi_isr(const struct device *dev)
{
	struct lynxi_dwcmshc_data *data = dev->data;
	uint32_t st = lynxi_readl(dev, SDHCI_INT_STATUS);

	if (st == 0) {
		return;
	}

	data->irq_last = st;
	data->error_code = (st >> 16) & 0xffffU;
	atomic_inc(&data->irq_count);
	data->irq_seen = true;

	if (st & (SDHCI_INT_ERROR | SDHCI_INT_DATA_END | SDHCI_INT_RESPONSE)) {
		k_sem_give(&data->irq_sem);
	}

	lynxi_writel(dev, st, SDHCI_INT_STATUS);
}
#endif

static void lynxi_resp_snapshot(const struct device *dev, uint32_t snap[4])
{
	for (int i = 0; i < 4; i++) {
		snap[i] = lynxi_readl(dev, SDHCI_RESPONSE + i * 4);
	}
}

static bool lynxi_resp_changed(const struct device *dev, const uint32_t snap[4],
			       uint32_t rt)
{
	if (rt == SD_RSP_TYPE_NONE) {
		return true;
	}
	if (rt == SD_RSP_TYPE_R2) {
		for (int i = 0; i < 4; i++) {
			if (lynxi_readl(dev, SDHCI_RESPONSE + i * 4) != snap[i]) {
				return true;
			}
		}
		return false;
	}

	return lynxi_readl(dev, SDHCI_RESPONSE) != snap[0];
}

/*
 * HE200 DWC: only CMD2 (ALL_SEND_CID) needs RESP latch verify after R3 OCR.
 * RT does not check stale RESP for CMD13 / other R1 traffic.
 */
static bool lynxi_need_resp_latch_check(const struct sdhc_command *cmd)
{
	return cmd->opcode == SD_ALL_SEND_CID;
}

/* R1b: wait DAT0 / inhibit release before next command (e.g. CMD13) */
static void lynxi_wait_dat0_ready(const struct device *dev, int timeout_ms)
{
	int64_t end = k_uptime_get() + (timeout_ms > 0 ? timeout_ms : 200);

	while (k_uptime_get() < end) {
		if (!lynxi_card_busy(dev)) {
			return;
		}
		k_busy_wait(50);
	}
}

/*
 * DWC MSHC may assert CC before RESP latches; wait until registers differ
 * from pre-command snapshot (CMD2 only on HE200).
 */
static int lynxi_wait_resp_latch(const struct device *dev, const uint32_t snap[4],
				 uint32_t rt, int timeout_ms)
{
	int64_t end = k_uptime_get() + (timeout_ms > 0 ? timeout_ms : 200);

	if (rt == SD_RSP_TYPE_NONE) {
		return 0;
	}

	while (k_uptime_get() < end) {
		if (lynxi_resp_changed(dev, snap, rt)) {
			return 0;
		}
		k_busy_wait(50);
	}

	lynxi_cmd_printk("stale RESP rt=%u snap[0]=%08x now=%08x\n", rt, snap[0],
			 lynxi_readl(dev, SDHCI_RESPONSE));
	return -EIO;
}

/*
 * RT sdhci_wait_command_done: rt_event_recv(ERROR|RESPONSE, FOREVER).
 * On ERROR (incl. CMD timeout) fail immediately; on RESPONSE read RESP.
 */
static int lynxi_wait_cmd_resp(const struct device *dev, int timeout_ms)
{
	struct lynxi_dwcmshc_data *dd = dev->data;
	int tmo = timeout_ms > 0 ? timeout_ms : 10000;

#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
	if (k_sem_take(&dd->irq_sem, K_MSEC(tmo)) != 0) {
		lynxi_cmd_printk("wait_cmd deadline present=0x%08x int=0x%08x\n",
				 lynxi_readl(dev, SDHCI_PRESENT_STATE),
				 lynxi_readl(dev, SDHCI_INT_STATUS));
		return -ETIMEDOUT;
	}
	if (dd->irq_last & SDHCI_INT_ERROR) {
		lynxi_cmd_printk("wait_cmd ERR st=0x%08x err=0x%04x\n", dd->irq_last,
				 dd->error_code);
		return -EIO;
	}
	lynxi_wait_inhibit_clear(dev, SDHCI_CMD_INHIBIT);
	return 0;
#else
	int64_t end = k_uptime_get() + tmo;

	while (k_uptime_get() < end) {
		uint32_t st = lynxi_readl(dev, SDHCI_INT_STATUS);

		if (st == 0) {
			k_busy_wait(10);
			continue;
		}
		if (st & SDHCI_INT_ERROR) {
			lynxi_writel(dev, st, SDHCI_INT_STATUS);
			lynxi_cmd_printk("wait_cmd ERR st=0x%08x\n", st);
			return -EIO;
		}
		if (st & SDHCI_INT_RESPONSE) {
			lynxi_writel(dev, st, SDHCI_INT_STATUS);
			lynxi_wait_inhibit_clear(dev, SDHCI_CMD_INHIBIT);
			return 0;
		}
		lynxi_writel(dev, st, SDHCI_INT_STATUS);
	}

	lynxi_cmd_printk("wait_cmd deadline present=0x%08x int=0x%08x\n",
			 lynxi_readl(dev, SDHCI_PRESENT_STATE),
			 lynxi_readl(dev, SDHCI_INT_STATUS));
	return -ETIMEDOUT;
#endif
}

static void lynxi_read_response(const struct device *dev, struct sdhc_command *cmd)
{
	uint32_t rt = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;

	if (rt == SD_RSP_TYPE_NONE) {
		return;
	}
	if (rt == SD_RSP_TYPE_R2) {
		/* RT sdhci_receive_command_response */
		for (int i = 0; i < 4; i++) {
			cmd->response[3 - i] = lynxi_readl(dev, SDHCI_RESPONSE + (3 - i) * 4) << 8;
			if (i != 3) {
				cmd->response[3 - i] |=
					lynxi_readb(dev, SDHCI_RESPONSE + (3 - i) * 4 - 1);
			}
		}
	} else if (rt == SD_RSP_TYPE_R3 || rt == SD_RSP_TYPE_R4) {
		/* RT sdhci_receive_command_response / lx_mmc_request: raw RESP[31:0] */
		cmd->response[0] = lynxi_readl(dev, SDHCI_RESPONSE);
		LOG_DBG("R3 raw=0x%08x", cmd->response[0]);
	} else {
		cmd->response[0] = lynxi_readl(dev, SDHCI_RESPONSE);
	}
}

/*
 * RT sdhci_set_transfer_config: rxData => read flag.
 * Zephyr sdhc_data has one buffer; infer direction from opcode.
 */
static bool lynxi_cmd_data_is_read(const struct sdhc_command *cmd, const struct sdhc_data *data)
{
	if (data == NULL) {
		return false;
	}

	switch (cmd->opcode) {
	case 24U: /* WRITE_SINGLE_BLOCK */
	case 25U: /* WRITE_MULTIPLE_BLOCK */
		return false;
	default:
		return true; /* incl. MMC_SEND_EXT_CSD (8) */
	}
}

static uint16_t lynxi_xfer_mode(struct sdhc_command *cmd, struct sdhc_data *data)
{
	uint16_t mode = 0;

	if (data == NULL) {
		return mode;
	}

	mode |= SDHCI_TRNS_BLK_CNT_EN;
	if (data->blocks > 1) {
		mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
	}
	if (lynxi_cmd_data_is_read(cmd, data)) {
		mode |= SDHCI_TRNS_READ;
	}
	return mode;
}

static uint16_t lynxi_cmd_flags(struct sdhc_command *cmd)
{
	uint16_t f = 0;
	uint32_t rt = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;

	switch (rt) {
	case SD_RSP_TYPE_NONE:
		break;
	case SD_RSP_TYPE_R2:
		f = SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC;
		break;
	case SD_RSP_TYPE_R1b:
		f = SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
		break;
	case SD_RSP_TYPE_R1:
	case SD_RSP_TYPE_R5:
	case SD_RSP_TYPE_R6:
	case SD_RSP_TYPE_R7:
		f = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
		break;
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:
		f = SDHCI_CMD_RESP_SHORT;
		break;
	default:
		break;
	}
	if (cmd->opcode == SD_STOP_TRANSMISSION) {
		f |= 0xc0;
	}
	return f;
}

/*
 * RT sdhci_transfer_data_blocking (PIO): poll PRESENT_STATE buffer ready + INT.
 * IRQ must be masked here — lynxi_isr clears INT_STATUS before poll can see it.
 */
static int lynxi_xfer_data_poll(const struct device *dev, struct sdhc_data *data, bool read)
{
	uint32_t block = 0;
	int timeout = 1000000;
	uint32_t ps_mask = read ? SDHCI_DATA_AVAILABLE : SDHCI_SPACE_AVAILABLE;

	while (1) {
		uint32_t st = lynxi_readl(dev, SDHCI_INT_STATUS);

		if (st & SDHCI_INT_ERROR) {
			lynxi_writel(dev, st, SDHCI_INT_STATUS);
			lynxi_cmd_printk("data ERR st=0x%08x\n", st);
			return -EIO;
		}

		if (st & (SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL)) {
			lynxi_writel(dev, st & (SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL),
				     SDHCI_INT_STATUS);
		}

		if (lynxi_readl(dev, SDHCI_PRESENT_STATE) & ps_mask) {
			for (uint32_t i = 0; i < data->block_size / 4; i++) {
				uint32_t off = block * data->block_size + i * 4;
				uint8_t *p = data->data;
				uint32_t w;

				if (read) {
					w = lynxi_readl(dev, SDHCI_BUFFER);
					memcpy(&p[off], &w, sizeof(w));
				} else {
					memcpy(&w, &p[off], sizeof(w));
					lynxi_writel(dev, w, SDHCI_BUFFER);
				}
			}
			block++;
			if (block >= data->blocks) {
				return 0;
			}
			continue;
		}

		if (st & SDHCI_INT_DATA_END) {
			lynxi_writel(dev, SDHCI_INT_DATA_END, SDHCI_INT_STATUS);
			return (block >= data->blocks) ? 0 : -EIO;
		}

		if (timeout-- == 0) {
			lynxi_cmd_printk("data timeout present=0x%08x int=0x%08x blk=%u/%u\n",
					 lynxi_readl(dev, SDHCI_PRESENT_STATE), st, block,
					 data->blocks);
			return -ETIMEDOUT;
		}
		k_busy_wait(5);
	}
}

static int lynxi_request(const struct device *dev, struct sdhc_command *cmd,
			 struct sdhc_data *data)
{
	struct lynxi_dwcmshc_data *dd = dev->data;
	uint16_t cmd_r, xfer;
	uint8_t ctrl;
	int ret;
	int timeout_ms = cmd->timeout_ms > 0 ? cmd->timeout_ms : 200;
	uint32_t rt = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;
	uint32_t resp_snap[4];
	bool read = false;
	int attempt;
	int max_attempts = (cmd->retries > 0) ? (cmd->retries + 1) : 3;
#if defined(CONFIG_LYNXI_DWCMSHC_CMD_DEBUG)
	int seq = (int)atomic_inc(&lynxi_cmd_seq) + 1;
#endif

	LOG_INF("CMD%u arg=0x%08x tmo=%d data=%p", cmd->opcode, cmd->arg, timeout_ms, data);
	lynxi_cmd_printk("CMD#%d op=%u(%s) arg=0x%08x tmo=%d blocks=%u\n", seq,
			 cmd->opcode, lynxi_opcode_name(cmd->opcode, cmd->arg), cmd->arg,
			 timeout_ms, data ? data->blocks : 0U);

	k_mutex_lock(&dd->bus_mutex, K_FOREVER);

	if (!dd->hw_inited) {
		lynxi_hw_init(dev);
	}

	if ((lynxi_readw(dev, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_CARD_EN) == 0) {
		(void)lynxi_clock_set(dev, 400000);
	}

	ret = -EIO;
	for (attempt = 0; attempt < max_attempts; attempt++) {
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
		k_sem_reset(&dd->irq_sem);
		dd->irq_last = 0;
		dd->error_code = 0;
#endif

		lynxi_wait_inhibit_clear(dev, SDHCI_CMD_INHIBIT);
		if (data) {
			lynxi_wait_inhibit_clear(dev, SDHCI_DATA_INHIBIT);
		}

		lynxi_writel(dev, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
		lynxi_resp_snapshot(dev, resp_snap);

		ctrl = lynxi_readb(dev, SDHCI_HOST_CONTROL);
		ctrl &= ~SDHCI_CTRL_DMA_MASK;
		lynxi_writeb(dev, ctrl, SDHCI_HOST_CONTROL);

		if (data) {
			/* RT sdhci_send_command: TIMEOUT_CONTROL only when data present */
			lynxi_writeb(dev, 0x0e, SDHCI_TIMEOUT_CONTROL);
			lynxi_writew(dev, SDHCI_MAKE_BLKSZ(7, data->block_size),
				     SDHCI_BLOCK_SIZE);
			lynxi_writew(dev, data->blocks, SDHCI_BLOCK_COUNT32);
			read = lynxi_cmd_data_is_read(cmd, data);
			if (!read && data->data) {
				sys_cache_data_flush_range(data->data,
							   data->block_size * data->blocks);
			}
		}

		xfer = lynxi_xfer_mode(cmd, data);
		cmd_r = SDHCI_MAKE_CMD(cmd->opcode, lynxi_cmd_flags(cmd));
		if (data) {
			cmd_r |= SDHCI_CMD_DATA;
		}

		lynxi_writel(dev, (lynxi_readl(dev, SDHCI_SIGNAL_ENABLE) &
				     ~(SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK)) |
				     SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK,
			     SDHCI_SIGNAL_ENABLE);

		/* RT sdhci_send_command: TRANSFER_MODE, ARGUMENT, COMMAND */
		lynxi_writew(dev, xfer, SDHCI_TRANSFER_MODE);
		lynxi_writel(dev, cmd->arg, SDHCI_ARGUMENT);
		lynxi_writew(dev, cmd_r, SDHCI_COMMAND);

		ret = lynxi_wait_cmd_resp(dev, timeout_ms);
		if (ret != 0) {
			lynxi_reset(dev, SDHCI_RESET_CMD);
			continue;
		}

		if (rt == SD_RSP_TYPE_R1b) {
			lynxi_wait_dat0_ready(dev, timeout_ms);
		}

		if (lynxi_need_resp_latch_check(cmd)) {
			ret = lynxi_wait_resp_latch(dev, resp_snap, rt, timeout_ms);
			if (ret != 0) {
				lynxi_reset(dev, SDHCI_RESET_CMD);
				continue;
			}
		}

		lynxi_read_response(dev, cmd);
		if (data) {
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
			irq_disable(((const struct lynxi_dwcmshc_config *)dev->config)->irq);
#endif
			ret = lynxi_xfer_data_poll(dev, data, read);
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
			irq_enable(((const struct lynxi_dwcmshc_config *)dev->config)->irq);
#endif
			if (ret == 0 && read && data->data) {
				sys_cache_data_invd_range(data->data,
							  data->block_size * data->blocks);
			}
			if (ret == 0) {
				lynxi_cmd_printk("CMD#%d op=%u data OK (%u bytes)\n", seq,
						 cmd->opcode,
						 data->block_size * data->blocks);
			}
		}
		if (ret == 0) {
			break;
		}
		lynxi_reset(dev, SDHCI_RESET_CMD);
	}

	lynxi_writel(dev, lynxi_readl(dev, SDHCI_SIGNAL_ENABLE) &
			     ~(SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK),
		     SDHCI_SIGNAL_ENABLE);
	if (ret != 0) {
		LOG_ERR("CMD%u failed %d int_st=0x%08x", cmd->opcode, ret,
			lynxi_readl(dev, SDHCI_INT_STATUS));
		lynxi_cmd_printk("CMD#%d op=%u FAIL ret=%d int_st=0x%08x\n", seq, cmd->opcode,
				 ret, lynxi_readl(dev, SDHCI_INT_STATUS));
#if defined(CONFIG_LYNXI_DWCMSHC_CMD_DEBUG)
		if (cmd->opcode == 2U) {
			lynxi_cmd_printk("CMD2 dbg present=0x%08x clk=0x%04x emmc=0x%08x hctl=0x%02x\n",
					 lynxi_readl(dev, SDHCI_PRESENT_STATE),
					 lynxi_readw(dev, SDHCI_CLOCK_CONTROL),
					 lynxi_readl(dev, EMMC_CTRL_R),
					 lynxi_readb(dev, SDHCI_HOST_CONTROL));
		}
#endif
	} else if (rt == SD_RSP_TYPE_R2) {
		LOG_INF("CMD%u OK R2", cmd->opcode);
		lynxi_cmd_printk("CMD#%d op=%u OK R2 %08x %08x %08x %08x\n", seq,
				 cmd->opcode, cmd->response[0], cmd->response[1],
				 cmd->response[2], cmd->response[3]);
	} else if (rt != SD_RSP_TYPE_NONE) {
		LOG_INF("CMD%u OK rsp0=0x%08x", cmd->opcode, cmd->response[0]);
		lynxi_cmd_printk("CMD#%d op=%u OK rsp0=0x%08x\n", seq, cmd->opcode,
				 cmd->response[0]);
	} else {
		LOG_INF("CMD%u OK", cmd->opcode);
		lynxi_cmd_printk("CMD#%d op=%u OK\n", seq, cmd->opcode);
	}
	lynxi_writel(dev, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
	lynxi_reset(dev, SDHCI_RESET_CMD);
	lynxi_reset(dev, SDHCI_RESET_DATA);
	k_mutex_unlock(&dd->bus_mutex);
	return ret;
}

static int lynxi_set_io(const struct device *dev, struct sdhc_io *io)
{
	struct lynxi_dwcmshc_data *data = dev->data;
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	uint8_t ctrl;
	uint32_t emmc;
	uint32_t hz = 400000;

	if (!data->hw_inited) {
		lynxi_hw_init(dev);
	}

	data->host_io = *io;
	data->timing = io->timing;

	switch (io->clock) {
	case SDMMC_CLOCK_400KHZ:
		hz = 400000;
		break;
	case MMC_CLOCK_26MHZ:
		hz = 26000000;
		break;
	case MMC_CLOCK_52MHZ:
		hz = 52000000;
		break;
	case MMC_CLOCK_HS200:
		hz = 200000000;
		break;
	default:
		hz = cfg->max_clk;
		break;
	}

	if (!cfg->is_emmc) {
		if (io->signal_voltage == SD_VOL_1_8_V) {
			uint16_t h2 = lynxi_readw(dev, SDHCI_HOST_CONTROL2);

			h2 |= SDHCI_CTRL_VDD_180;
			lynxi_writew(dev, h2, SDHCI_HOST_CONTROL2);
		}
		lynxi_set_uhs_signaling(dev, io->timing);
	}

	if (lynxi_clock_set(dev, hz) != 0) {
		return -EIO;
	}

	ctrl = lynxi_readb(dev, SDHCI_HOST_CONTROL);
	ctrl &= ~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS);
	if (io->bus_width == SDHC_BUS_WIDTH8BIT) {
		ctrl |= SDHCI_CTRL_8BITBUS;
	} else if (io->bus_width == SDHC_BUS_WIDTH4BIT) {
		ctrl |= SDHCI_CTRL_4BITBUS;
	}
	lynxi_writeb(dev, ctrl, SDHCI_HOST_CONTROL);

	emmc = lynxi_readl(dev, EMMC_CTRL_R);
	if (cfg->is_emmc) {
		emmc |= BIT(CARD_IS_EMMC);
	} else {
		emmc &= ~BIT(CARD_IS_EMMC);
	}
	lynxi_writel(dev, emmc, EMMC_CTRL_R);

	return 0;
}

static int lynxi_get_card_present(const struct device *dev)
{
	const struct lynxi_dwcmshc_config *cfg = dev->config;

	if (cfg->is_emmc) {
		return 1;
	}
	return (lynxi_readl(dev, SDHCI_PRESENT_STATE) & BIT(16)) ? 1 : 0;
}

static int lynxi_get_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	struct lynxi_dwcmshc_data *data = dev->data;

	*props = data->props;
	return 0;
}

static int lynxi_reset_api(const struct device *dev)
{
	lynxi_reset(dev, SDHCI_RESET_ALL);
	return 0;
}

/*
 * RT has no executeTuning CMD loop; HS200 relies on lx_mmc_clock_freq_change()
 * (lyn_sdhci_dwcmshc_dl_cfg + 200MHz). Zephyr mmc.c calls sdhc_execute_tuning
 * after set_io — re-apply delay lines and succeed.
 */
static int lynxi_execute_tuning(const struct device *dev)
{
	struct lynxi_dwcmshc_data *data = dev->data;

	if (data->timing != SDHC_TIMING_HS200) {
		return 0;
	}

	lynxi_dl_cfg(dev);
	lynxi_at_vendor_init(dev);
	LOG_DBG("HS200 tuning: dl_cfg tap path (RT-style, no CMD19)");
	return 0;
}

static DEVICE_API(sdhc, lynxi_dwcmshc_api) = {
	.request = lynxi_request,
	.set_io = lynxi_set_io,
	.execute_tuning = lynxi_execute_tuning,
	.get_card_present = lynxi_get_card_present,
	.card_busy = lynxi_card_busy,
	.get_host_props = lynxi_get_host_props,
	.reset = lynxi_reset_api,
};

#if defined(CONFIG_LYNXI_DWCMSHC_IRQ_SELFTEST)
static int lynxi_irq_selftest(const struct device *dev)
{
	struct lynxi_dwcmshc_data *data = dev->data;
	struct sdhc_command cmd = {
		.opcode = 0,
		.arg = 0,
		.response_type = SD_RSP_TYPE_NONE,
		.timeout_ms = 50,
		.retries = 0,
	};
	int ret;
	uint32_t cnt;

	printk("lynxi-sdhc: IRQ self-test start\n");

	atomic_clear(&data->irq_count);
	data->irq_seen = false;
	k_sem_reset(&data->irq_sem);

	ret = lynxi_request(dev, &cmd, NULL);

	cnt = (uint32_t)atomic_get(&data->irq_count);
	printk("lynxi-sdhc: IRQ self-test CMD0 ret=%d irq_cnt=%u seen=%d\n", ret, cnt,
	       data->irq_seen ? 1 : 0);

	lynxi_writel(dev, 0, SDHCI_SIGNAL_ENABLE);
	return (ret == 0 && data->irq_seen) ? 0 : -EIO;
}
#endif

static int lynxi_dwcmshc_init(const struct device *dev)
{
	const struct lynxi_dwcmshc_config *cfg = dev->config;
	struct lynxi_dwcmshc_data *data = dev->data;
	int ret = 0;

	k_mutex_init(&data->bus_mutex);
#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
	/* RT rt_event OR-accumulates; sem must not drop back-to-back IRQs */
	k_sem_init(&data->irq_sem, 0, 32);
	atomic_clear(&data->irq_count);
	data->irq_seen = false;
	if (cfg->irq_config != NULL) {
		cfg->irq_config(dev);
		printk("lynxi-sdhc: GIC IRQ %u connected\n", DT_INST_IRQN(0));
	}
#endif
	data->props.f_max = cfg->max_clk;
	data->props.f_min = cfg->min_clk;
	data->props.power_delay = cfg->power_delay_ms;
	data->props.bus_4_bit_support = true;
	data->props.hs200_support = cfg->hs200;
	data->props.is_spi = false;
	data->props.max_current_330 = 500;
	data->props.max_current_180 = 500;
	data->props.host_caps.bus_8_bit_support = cfg->is_emmc ? 1 : 0;
	data->props.host_caps.vol_180_support = cfg->io_1v8 ? 1 : 0;
	data->props.host_caps.vol_330_support = cfg->io_1v8 ? 0 : 1;
	data->props.host_caps.high_spd_support = 1;
	if (cfg->is_emmc) {
		/* SDHCI slot type 1 = embedded (eMMC) */
		data->props.host_caps.slot_type = 1;
	}

	lynxi_hw_init(dev);

	if (cfg->is_emmc) {
		printk("lynxi-sdhc: eMMC host — MMC protocol (no SD CMD8 probe)\n");
	}

#if defined(CONFIG_LYNXI_DWCMSHC_DEBUG)
	lynxi_dbg_dump(dev, "after hw_init");
	printk("lynxi-sdhc: init done (irq_selftest=%s)\n",
	       IS_ENABLED(CONFIG_LYNXI_DWCMSHC_IRQ_SELFTEST) ? "on" : "off");
#endif

#if defined(CONFIG_LYNXI_DWCMSHC_IRQ_SELFTEST)
	if (lynxi_irq_selftest(dev) != 0) {
		printk("lynxi-sdhc: WARN IRQ self-test failed (poll fallback)\n");
	}
#endif

	return ret;
}

#ifdef CONFIG_LYNXI_DWCMSHC_INTERRUPT
#define LYNXI_DWCMSHC_IRQ_CONNECT(n)						\
	static void lynxi_irq_config_##n(const struct device *dev)		\
	{									\
		ARG_UNUSED(dev);						\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    lynxi_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}
#define LYNXI_DWCMSHC_IRQ_CFG(n) .irq_config = lynxi_irq_config_##n,
#else
#define LYNXI_DWCMSHC_IRQ_CONNECT(n)
#define LYNXI_DWCMSHC_IRQ_CFG(n)
#endif

#define LYNXI_DWCMSHC_INIT(n)							\
	LYNXI_DWCMSHC_IRQ_CONNECT(n)						\
	static const struct lynxi_dwcmshc_config lynxi_dwcmshc_cfg_##n = {	\
		.reg_base = DT_INST_REG_ADDR(n),					\
		.max_clk = DT_INST_PROP(n, max_bus_freq),				\
		.min_clk = DT_INST_PROP(n, min_bus_freq),				\
		.power_delay_ms = DT_INST_PROP(n, power_delay_ms),			\
		.is_emmc = DT_INST_PROP(n, lynxi_emmc),					\
		.io_1v8 = DT_INST_PROP(n, lynxi_io_1v8),				\
		.hs200 = DT_INST_PROP_OR(n, mmc_hs200_1_8v, 0),			\
		LYNXI_DWCMSHC_IRQ_CFG(n)						\
		COND_CODE_1(CONFIG_LYNXI_DWCMSHC_INTERRUPT,				\
			    (.irq = DT_INST_IRQN(n),), ())				\
	};									\
	static struct lynxi_dwcmshc_data lynxi_dwcmshc_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, lynxi_dwcmshc_init, NULL,				\
			      &lynxi_dwcmshc_data_##n, &lynxi_dwcmshc_cfg_##n,	\
			      POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY,		\
			      &lynxi_dwcmshc_api);

DT_INST_FOREACH_STATUS_OKAY(LYNXI_DWCMSHC_INIT)
