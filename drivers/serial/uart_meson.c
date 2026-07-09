/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Amlogic Meson UART driver for S4 (S905Y4/S905W2) and later SoCs.
 */

#define DT_DRV_COMPAT amlogic_meson_s4_uart

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/interrupt_controller/gic.h>

LOG_MODULE_REGISTER(uart_meson, CONFIG_UART_LOG_LEVEL);

#define UART_WFIFO       0x00U
#define UART_RFIFO       0x04U
#define UART_CONTROL     0x08U
#define UART_STATUS      0x0CU
#define UART_MISC        0x10U
#define UART_REG5        0x14U

#define TX_EN            (1U << 12)
#define RX_EN            (1U << 13)
#define TX_RST           (1U << 22)
#define RX_RST           (1U << 23)
#define CLR_ERR          (1U << 24)
#define RX_INT_EN        (1U << 27)
#define TX_INT_EN        (1U << 28)
#define UART_INIT_MASK   (TX_RST | RX_RST | CLR_ERR | TX_EN | RX_EN)

#define MISC_RECV_IRQ(c) ((c) & 0xffU)
#define MISC_XMIT_IRQ(c) (((c) & 0xffU) << 8)
#define UART_FIFO_SIZE   64U

#define RX_EMPTY         (1U << 20)
#define TX_FULL          (1U << 21)
#define TX_EMPTY         (1U << 22)
#define UART_RX_ERR      ((1U << 16) | (1U << 17) | (1U << 18))

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

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
#define S4_UART_B_PHYS  0xFE07A000U

static uint32_t meson_uart_read(const struct device *dev, uint32_t offset)
{
	ARG_UNUSED(dev);
	return sys_read32(S4_UART_B_PHYS + offset);
}

static void meson_uart_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	ARG_UNUSED(dev);
	sys_write32(value, S4_UART_B_PHYS + offset);
}

static void meson_s4_uart_irq_prepare(void)
{
	uint32_t ctrl = meson_uart_read(NULL, UART_CONTROL);

	/* Do not reset FIFOs — U-Boot/early UART already configured baud. */
	ctrl |= CLR_ERR;
	meson_uart_write(NULL, UART_CONTROL, ctrl);
	ctrl &= ~CLR_ERR;
	meson_uart_write(NULL, UART_CONTROL, ctrl);

	ctrl = meson_uart_read(NULL, UART_CONTROL);
	ctrl |= (TX_EN | RX_EN);
	ctrl &= ~(RX_INT_EN | TX_INT_EN);
	meson_uart_write(NULL, UART_CONTROL, ctrl);

	meson_uart_write(NULL, UART_MISC,
			 MISC_RECV_IRQ(1U) | MISC_XMIT_IRQ(UART_FIFO_SIZE / 2U));
}
#else
static inline uint32_t meson_uart_read(const struct device *dev, uint32_t offset)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + offset);
}

static inline void meson_uart_write(const struct device *dev, uint32_t offset, uint32_t value)
{
	sys_write32(value, DEVICE_MMIO_GET(dev) + offset);
}
#endif

static void meson_uart_clear_errors(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	ctrl |= CLR_ERR;
	meson_uart_write(dev, UART_CONTROL, ctrl);
	ctrl &= ~CLR_ERR;
	meson_uart_write(dev, UART_CONTROL, ctrl);
}

static int meson_uart_poll_in(const struct device *dev, unsigned char *p_char)
{
	uint32_t status = meson_uart_read(dev, UART_STATUS);

	if (status & RX_EMPTY) {
		return -1;
	}

	if (status & UART_RX_ERR) {
		meson_uart_clear_errors(dev);
	}

	*p_char = (unsigned char)(meson_uart_read(dev, UART_RFIFO) & 0xFF);
	return 0;
}

static void meson_uart_poll_out(const struct device *dev, unsigned char out_char)
{
	uint32_t status;

	do {
		status = meson_uart_read(dev, UART_STATUS);
	} while (status & TX_FULL);

	meson_uart_write(dev, UART_WFIFO, (uint32_t)out_char);

	do {
		status = meson_uart_read(dev, UART_STATUS);
	} while ((status & TX_EMPTY) == 0U);
}

static int meson_uart_err_check(const struct device *dev)
{
	return 0;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static int meson_uart_irq_tx_ready(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	if ((ctrl & TX_INT_EN) == 0U) {
		return 0;
	}

	return !(meson_uart_read(dev, UART_STATUS) & TX_FULL);
}

static int meson_uart_irq_rx_ready(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	if ((ctrl & RX_INT_EN) == 0U) {
		return 0;
	}

	return !(meson_uart_read(dev, UART_STATUS) & RX_EMPTY);
}

static int meson_uart_irq_is_pending(const struct device *dev)
{
	return meson_uart_irq_tx_ready(dev) || meson_uart_irq_rx_ready(dev);
}

static void meson_uart_irq_callback_invoke(const struct device *dev)
{
	struct meson_uart_data *data = dev->data;

	if (data->irq_cb != NULL) {
		data->irq_cb(dev, data->irq_cb_data);
	}
}

static void meson_uart_misc_set(const struct device *dev)
{
	meson_uart_write(dev, UART_MISC,
			 MISC_RECV_IRQ(1U) | MISC_XMIT_IRQ(UART_FIFO_SIZE / 2U));
}

static void meson_uart_stop_tx(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	ctrl &= ~TX_INT_EN;
	meson_uart_write(dev, UART_CONTROL, ctrl);
}

static void meson_uart_start_tx(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	ctrl |= TX_EN | TX_INT_EN;
	meson_uart_write(dev, UART_CONTROL, ctrl);
	meson_uart_misc_set(dev);
	meson_uart_irq_callback_invoke(dev);
}

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

	/* Linux start_tx: keep TX_INT_EN if burst did not drain the whole buffer. */
	if (i < size) {
		uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

		ctrl |= TX_EN | TX_INT_EN;
		meson_uart_write(dev, UART_CONTROL, ctrl);
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
		if (status & UART_RX_ERR) {
			meson_uart_clear_errors(dev);
		}
		rx_data[i] = (uint8_t)(meson_uart_read(dev, UART_RFIFO) & 0xFF);
	}

	return i;
}

static void meson_uart_irq_tx_enable(const struct device *dev)
{
	/* Align Linux meson_uart_start_tx: burst via callback, then TX INT if needed. */
	meson_uart_start_tx(dev);
}

static void meson_uart_irq_tx_disable(const struct device *dev)
{
	meson_uart_stop_tx(dev);
}

static void meson_uart_irq_rx_enable(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	ctrl |= RX_EN | RX_INT_EN;
	meson_uart_write(dev, UART_CONTROL, ctrl);
	meson_uart_misc_set(dev);

	if (meson_uart_irq_rx_ready(dev)) {
		meson_uart_irq_callback_invoke(dev);
	}
}

static void meson_uart_irq_rx_disable(const struct device *dev)
{
	uint32_t ctrl = meson_uart_read(dev, UART_CONTROL);

	ctrl &= ~RX_INT_EN;
	meson_uart_write(dev, UART_CONTROL, ctrl);
}

static int meson_uart_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);
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

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
#ifdef CONFIG_SOC_MESON_S4_SHELL_SMP
uint32_t meson_uart_isr_count;
#endif

static void meson_uart_isr(const struct device *dev)
{
#ifdef CONFIG_SOC_MESON_S4_SHELL_SMP
	meson_uart_isr_count++;
#endif
	/* Linux meson_uart_interrupt: RX first, then TX if !TX_FULL && TX_INT_EN. */
	if (meson_uart_irq_rx_ready(dev) || meson_uart_irq_tx_ready(dev)) {
		meson_uart_irq_callback_invoke(dev);
	}
}

static void meson_uart_s4_irq_setup(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority),
		    meson_uart_isr, DEVICE_DT_INST_GET(0), DT_INST_IRQ(0, flags));
	irq_enable(DT_INST_IRQN(0));
}
#endif /* CONFIG_SOC_AMLOGIC_MESON_S4 */

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int meson_uart_init(const struct device *dev)
{
#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	ARG_UNUSED(dev);
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	meson_uart_s4_irq_setup();
#endif
	return 0;
#else
	const struct meson_uart_config *cfg = dev->config;
	uint32_t ctrl;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	ctrl = meson_uart_read(dev, UART_CONTROL);
	ctrl |= UART_INIT_MASK;
	meson_uart_write(dev, UART_CONTROL, ctrl);

	ctrl &= ~(TX_RST | RX_RST | CLR_ERR);
	meson_uart_write(dev, UART_CONTROL, ctrl);

	ctrl |= (TX_EN | RX_EN);
	meson_uart_write(dev, UART_CONTROL, ctrl);

	meson_uart_write(dev, UART_MISC,
			 MISC_RECV_IRQ(1U) | MISC_XMIT_IRQ(UART_FIFO_SIZE / 2U));

	if (cfg->baud_rate > 0 && cfg->clock_freq > 0) {
		uint32_t xtal_div = 1;
		uint32_t divisor = cfg->clock_freq / (xtal_div * 8U * cfg->baud_rate);

		if (divisor > 0) {
			divisor -= 1;
		}

		meson_uart_write(dev, UART_REG5,
				 (xtal_div << REG5_XTAL_DIV_SHIFT) |
				 (divisor & 0xFFFF) |
				 REG5_USE_NEW_RATE |
				 REG5_USE_XTAL_CLK);
	}

	return 0;
#endif
}

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4) && defined(CONFIG_UART_INTERRUPT_DRIVEN)
static void meson_s4_gic_force_edge_rising(unsigned int irq)
{
	unsigned int grp = (irq / 16U) * 4U;
	unsigned int off = (irq % 16U) * 2U;
	uint32_t val = sys_read32(GICD_ICFGRn + grp);

	val &= ~(GICD_ICFGR_MASK << off);
	val |= (GICD_ICFGR_TYPE << off);
	sys_write32(val, GICD_ICFGRn + grp);
}

static int meson_s4_uart_post_init(void)
{
	meson_s4_uart_irq_prepare();
	meson_s4_gic_force_edge_rising(DT_INST_IRQN(0));
	return 0;
}

SYS_INIT(meson_s4_uart_post_init, POST_KERNEL, 80);
#endif

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
