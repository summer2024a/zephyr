/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#define HE200_UART0_BASE            0x10006000U
#define HE200_UART_REF_CLK_HZ       50000000U
#define HE200_UART_BAUD             115200U

#define HE200_UART_RBR              0x00U
#define HE200_UART_IER              0x04U
#define HE200_UART_IIR              0x08U
#define HE200_UART_LCR              0x0cU
#define HE200_UART_MCR              0x10U
#define HE200_UART_LSR              0x14U
#define HE200_UART_LCR_DLAB         0x80U
#define HE200_UART_LSR_THRE         0x20U

static volatile uint32_t *const he200_uart_thr =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_RBR);
static volatile uint32_t *const he200_uart_ier =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_IER);
static volatile uint32_t *const he200_uart_iir =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_IIR);
static volatile uint32_t *const he200_uart_lcr =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_LCR);
static volatile uint32_t *const he200_uart_mcr =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_MCR);
static volatile uint32_t *const he200_uart_lsr =
	(volatile uint32_t *)(HE200_UART0_BASE + HE200_UART_LSR);

static void he200_uart_early_init(void)
{
	uint32_t ibrd;
	uint32_t lcr;

	*he200_uart_lcr = 0x3U;
	*he200_uart_ier = (*he200_uart_ier & 0x40U);
	*he200_uart_iir = 0U;
	*he200_uart_mcr = 0x3U;

	ibrd = ((HE200_UART_REF_CLK_HZ / HE200_UART_BAUD) * 1000U / 16U) / 1000U;
	lcr = *he200_uart_lcr;
	*he200_uart_lcr = lcr | HE200_UART_LCR_DLAB;
	*he200_uart_thr = ibrd & 0xffU;
	*he200_uart_ier = (ibrd >> 8) & 0xffU;
	*he200_uart_lcr = lcr & ~HE200_UART_LCR_DLAB;
}

static void he200_uart_putc(char c)
{
	while ((*he200_uart_lsr & HE200_UART_LSR_THRE) == 0U) {
		;
	}

	*he200_uart_thr = (uint32_t)c;
}

void he200_ep_boot_marker(char tag)
{
	he200_uart_putc(tag);
}

static void he200_ep_boot_stage(const char *msg)
{
	while (*msg != '\0') {
		he200_uart_putc(*msg++);
	}
	he200_uart_putc('\r');
	he200_uart_putc('\n');
}

static unsigned int he200_read_el(void)
{
	uint64_t reg;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(reg));
	return (unsigned int)((reg >> 2) & 3U);
}

void he200_ep_prep_trace(char step)
{
	he200_ep_boot_marker(step);
}

void he200_ep_boot_debug_highest(void)
{
	static bool uart_ready;

	if (!uart_ready) {
		he200_uart_early_init();
		uart_ready = true;
		he200_ep_boot_stage("Zephyr SPL");
	}

	he200_ep_boot_marker((char)('0' + he200_read_el()));
}
