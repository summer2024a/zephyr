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
 */

#define DT_DRV_COMPAT amlogic_meson_s4_uart

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/device_mmio.h>
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

/* REG5 baud rate control */
#define REG5_XTAL_DIV_SHIFT  0U
#define REG5_USE_NEW_RATE    (1U << 23)
#define REG5_USE_XTAL_CLK    (1U << 24)

struct meson_uart_config {
	DEVICE_MMIO_ROM;
	uint32_t clock_freq;
	uint32_t baud_rate;
};

struct meson_uart_data {
	DEVICE_MMIO_RAM;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t irq_cb;
	void *irq_cb_data;
#endif
};

static inline uint32_t meson_uart_read(const struct device *dev, uint32_t offset)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + offset);
}

static inline void meson_uart_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	sys_write32(value, DEVICE_MMIO_GET(dev) + offset);
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
	return 0;
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
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) | TX_EN);
}

static void meson_uart_irq_tx_disable(const struct device *dev)
{
	meson_uart_write(dev, UART_CONTROL,
			 meson_uart_read(dev, UART_CONTROL) & ~TX_EN);
}

static int meson_uart_irq_tx_ready(const struct device *dev)
{
	return !(meson_uart_read(dev, UART_STATUS) & TX_FULL);
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
	return !(meson_uart_read(dev, UART_STATUS) & RX_EMPTY);
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

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

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
	 * baud = xtal_clk / (xtal_div * 8 * (reg5_baud + 1))
	 * For 115200 from 24MHz: xtal_div=1, reg5_baud = 24000000/(8*115200) - 1 = 25
	 * actual baud ≈ 24000000/(8*26) = 115384
	 */
	if (cfg->baud_rate > 0 && cfg->clock_freq > 0) {
		uint32_t xtal_div = 1;
		uint32_t divisor = cfg->clock_freq / (xtal_div * 8U * cfg->baud_rate);

		if (divisor > 0) {
			divisor -= 1;
		}

		uint32_t reg5_val = (xtal_div << REG5_XTAL_DIV_SHIFT) |
				    (divisor & 0xFFFF) |
				    REG5_USE_NEW_RATE |
				    REG5_USE_XTAL_CLK;
		meson_uart_write(dev, UART_REG5, reg5_val);
	}

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
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                  \
		.clock_freq = DT_INST_PROP(inst, clock_frequency),        \
		.baud_rate = DT_INST_PROP(inst, current_speed),           \
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