/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Amlogic Meson UART driver for S4 (S905Y4/S905W2) and later SoCs.
 *
 * Register layout (different from ns16550/DW APB UART):
 *   WFIFO   +0x00  — write character
 *   RFIFO   +0x04  — read received character
 *   CONTROL +0x08  — control register
 *   STATUS  +0x0C  — status register
 *   MISC    +0x10  — miscellaneous / baud rate divider
 *   REG5    +0x14  — new baud rate control
 *
 * CONTROL bits:
 *   bit 12: TX_EN
 *   bit 13: RX_EN
 *   bit 22: TX_RST (auto-clear)
 *   bit 23: RX_RST (auto-clear)
 *   bit 24: CLR_ERR (auto-clear)
 *
 * STATUS bits:
 *   bit 20: RX_EMPTY
 *   bit 21: TX_FULL
 *   bit 22: TX_EMPTY
 *   bit 25: XMIT_BUSY
 *
 * Reference: Linux drivers/serial/serial_meson.c, baremetal_uart.S
 */

#define DT_DRV_COMPAT amlogic_meson_s4_uart

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

LOG_MODULE_REGISTER(uart_meson, CONFIG_UART_LOG_LEVEL);

/* Register offsets */
#define UART_WFIFO       0x00U
#define UART_RFIFO       0x04U
#define UART_CONTROL     0x08U
#define UART_STATUS       0x0CU
#define UART_MISC         0x10U
#define UART_REG5         0x14U

/* CONTROL register bits */
#define TX_EN             (1U << 12)
#define RX_EN             (1U << 13)
#define TX_RST            (1U << 22)
#define RX_RST            (1U << 23)
#define CLR_ERR           (1U << 24)
#define UART_INIT_MASK    (TX_RST | RX_RST | CLR_ERR | TX_EN | RX_EN)

/* STATUS register bits */
#define RX_EMPTY          (1U << 20)
#define TX_FULL           (1U << 21)
#define TX_EMPTY          (1U << 22)
#define XMIT_BUSY         (1U << 25)

/* MISC register — baud rate for older Meson UARTs (not S4 REG5 mode) */
#define UART_MISC_BAUD_EXT_MASK   0x7U
#define UART_MISC_BAUD_EXT_SHIFT  0U
#define UART_MISC_BAUD_MASK       0xFFFU
#define UART_MISC_BAUD_SHIFT      0U

/* REG5 — new baud rate control for S4 */
#define UART_REG5_XTAL_DIV_MASK   0x7U
#define UART_REG5_XTAL_DIV_SHIFT  0U
#define UART_REG5_USE_XTAL_CLK    (1U << 24)
#define UART_REG5_USE_NEW_RATE    (1U << 23)

struct meson_uart_config {
	MMIO_MMIO8_CALLBACKS_TYPE;
	uint32_t base;
	uint32_t irq;
	uint32_t clock_freq;   /* XTAL frequency (24MHz) */
	uint32_t baud_rate;
};

struct meson_uart_data {
	uint8_t *rx_buf;
	uint16_t rx_buf_len;
	uint16_t rx_buf_head;
	uint16_t rx_buf_tail;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t irq_cb;
	void *irq_cb_data;
#endif
};

static inline uint32_t meson_uart_read(const struct device *dev, uint32_t offset)
{
	const struct meson_uart_config *cfg = dev->config;

	return sys_read32(cfg->base + offset);
}

static inline void meson_uart_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	const struct meson_uart_config *cfg = dev->config;

	sys_write32(value, cfg->base + offset);
}

static int meson_uart_poll_in(const struct device *dev, unsigned char *p_char)
{
	uint32_t status;

	status = meson_uart_read(dev, UART_STATUS);
	if (status & RX_EMPTY) {
		return -1;
	}

	*p_char = (unsigned char)(meson_uart_read(dev, UART_RFIFO) & 0xFF);
	return 0;
}

static void meson_uart_poll_out(const struct device *dev, unsigned char out_char)
{
	uint32_t status;

	/* Wait until TX_FULL is clear */
	do {
		status = meson_uart_read(dev, UART_STATUS);
	} while (status & TX_FULL);

	meson_uart_write(dev, UART_WFIFO, (uint32_t)out_char);
}

static int meson_uart_err_check(const struct device *dev)
{
	uint32_t status;
	int errors = 0;

	status = meson_uart_read(dev, UART_STATUS);

	/* Meson UART STATUS doesn't have standard error bits like ns16550.
	 * Check for unusual conditions — RX_EMPTY means no data, XMIT_BUSY
	 * means TX still sending. For now, return 0 errors. */
	return errors;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static int meson_uart_fifo_fill(const struct device *dev,
				const uint8_t *tx_data, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		uint32_t status = meson_uart_read(dev, UART_STATUS);

		if (status & TX_FULL) {
			break;
		}
		meson_uart_write(dev, UART_WFIFO, (uint32_t)tx_data[i]);
	}

	return i;
}

static int meson_uart_fifo_read(const struct device *dev,
				uint8_t *rx_data, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		uint32_t status = meson_uart_read(dev, UART_STATUS);

		if (status & RX_EMPTY) {
			break;
		}
		rx_data[i] = (uint8_t)(meson_uart_read(dev, UART_RFIFO) & 0xFF);
	}

	return i;
}

static void meson_uart_irq_tx_enable(const struct device *dev)
{
	/* Meson UART doesn't have a dedicated TX interrupt enable bit.
	 * The CONTROL register only has TX_EN/RX_EN for FIFO path enable.
	 * For interrupt-driven TX, we rely on checking TX_EMPTY/TX_FULL
	 * in STATUS. Enable the global UART interrupt. */
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) | TX_EN);
}

static void meson_uart_irq_tx_disable(const struct device *dev)
{
	/* Disable TX path — note: we keep RX_EN for console input */
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) & ~TX_EN);
}

static int meson_uart_irq_tx_ready(const struct device *dev)
{
	uint32_t status = meson_uart_read(dev, UART_STATUS);

	return !(status & TX_FULL);
}

static void meson_uart_irq_rx_enable(const struct device *dev)
{
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) | RX_EN);
}

static void meson_uart_irq_rx_disable(const struct device *dev)
{
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) & ~RX_EN);
}

static int meson_uart_irq_rx_ready(const struct device *dev)
{
	uint32_t status = meson_uart_read(dev, UART_STATUS);

	return !(status & RX_EMPTY);
}

static int meson_uart_irq_is_pending(const struct device *dev)
{
	return meson_uart_irq_rx_ready(dev) || meson_uart_irq_tx_ready(dev);
}

static int meson_uart_irq_update(const struct device *dev)
{
	return 1;
}

static void meson_uart_irq_callback_set(const struct device *dev,
					 uart_irq_callback_user_data_t cb,
					 void *cb_data)
{
	struct meson_uart_data *data = dev->data;

	data->irq_cb = cb;
	data->irq_cb_data = cb_data;
}

static void meson_uart_isr(const struct device *dev)
{
	struct meson_uart_data *data = dev->data;

	if (data->irq_cb) {
		data->irq_cb(dev, data->irq_cb_data);
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int meson_uart_init(const struct device *dev)
{
	uint32_t ctrl;
	const struct meson_uart_config *cfg = dev->config;

	/* Reset TX/RX FIFOs, clear errors, then enable TX/RX */
	ctrl = meson_uart_read(dev, UART_CONTROL);
	ctrl |= UART_INIT_MASK;
	meson_uart_write(dev, UART_CONTROL, ctrl);

	/* Pulse bits auto-clear; clear reset/error bits explicitly */
	ctrl &= ~(TX_RST | RX_RST | CLR_ERR);
	meson_uart_write(dev, UART_CONTROL, ctrl);

	/* Enable TX and RX */
	ctrl |= (TX_EN | RX_EN);
	meson_uart_write(dev, UART_CONTROL, ctrl);

	/* Configure baud rate using REG5 (S4 new rate mode)
	 *
	 * For S4: baud_rate = xtal_clk / (xtal_div * 8 * (reg5_baud + 1))
	 * We use XTAL 24MHz and set xtal_div and reg5_baud for target baud.
	 * For 115200: xtal_div=1, then reg5_baud = (24000000/(1*8*115200)) - 1
	 *            reg5_baud = 24000000/921600 - 1 = 25.04 → 25
	 *            actual baud ≈ 24000000/(8*26) = 115384 (close enough)
	 */
	if (cfg->baud_rate > 0 && cfg->clock_freq > 0) {
		uint32_t xtal_div = 1; /* xtal_div=1 gives best precision for common bauds */
		uint32_t divisor = cfg->clock_freq / (xtal_div * 8U * cfg->baud_rate);
		uint32_t reg5_val;

		if (divisor > 0) {
			divisor -= 1; /* REG5 baud = (divisor - 1) */
		}

		reg5_val = (xtal_div << UART_REG5_XTAL_DIV_SHIFT) |
			   (divisor & 0xFFFF) |
			   UART_REG5_USE_NEW_RATE |
			   UART_REG5_USE_XTAL_CLK;
		meson_uart_write(dev, UART_REG5, reg5_val);
	}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	IRQ_CONNECT(cfg->irq, 0, meson_uart_isr, dev, 0);
	irq_enable(cfg->irq);
#endif

	return 0;
}

static const struct uart_driver_api meson_uart_driver_api = {
	.poll_in = meson_uart_poll_in,
	.poll_out = meson_uart_poll_out,
	.err_check = meson_uart_err_check,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = meson_uart_fifo_fill,
	.fifo_read = meson_uart_fifo_read,
	.irq_tx_enable = meson_uart_irq_tx_enable,
	.irq_tx_disable = meson_uart_irq_tx_disable,
	.irq_tx_ready = meson_uart_irq_tx_ready,
	.irq_rx_enable = meson_uart_irq_rx_enable,
	.irq_rx_disable = meson_uart_irq_rx_disable,
	.irq_rx_ready = meson_uart_irq_rx_ready,
	.irq_is_pending = meson_uart_irq_is_pending,
	.irq_update = meson_uart_irq_update,
	.irq_callback_set = meson_uart_irq_callback_set,
#endif
};

#define MESON_UART_INIT(inst)                                          \
	static struct meson_uart_data meson_uart_data_##inst;          \
	static const struct meson_uart_config meson_uart_config_##inst = { \
		.base = DT_INST_REG_ADDR(inst),                            \
		.irq = DT_INST_IRQN(inst),                                 \
		.clock_freq = DT_INST_PROP(inst, clock_frequency),         \
		.baud_rate = DT_INST_PROP(inst, current_speed),            \
	};                                                              \
	DEVICE_DT_INST_DEFINE(inst,                                     \
			      meson_uart_init,                              \
			      NULL,                                         \
			      &meson_uart_data_##inst,                       \
			      &meson_uart_config_##inst,                     \
			      PRE_KERNEL_1,                                 \
			      CONFIG_SERIAL_INIT_PRIORITY,                  \
			      &meson_uart_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MESON_UART_INIT)