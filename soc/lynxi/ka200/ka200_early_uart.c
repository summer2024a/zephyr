/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#define KA200_UART0_BASE            0x10006000U
#define KA200_UART_REF_CLK_HZ       50000000U
#define KA200_UART_BAUD             115200U

#define KA200_UART_RBR              0x00U
#define KA200_UART_IER              0x04U
#define KA200_UART_IIR              0x08U
#define KA200_UART_LCR              0x0cU
#define KA200_UART_MCR              0x10U
#define KA200_UART_LSR              0x14U
#define KA200_UART_LCR_DLAB         0x80U
#define KA200_UART_LSR_THRE         0x20U

static volatile uint32_t *const ka200_uart_thr =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_RBR);
static volatile uint32_t *const ka200_uart_ier =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_IER);
static volatile uint32_t *const ka200_uart_iir =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_IIR);
static volatile uint32_t *const ka200_uart_lcr =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_LCR);
static volatile uint32_t *const ka200_uart_mcr =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_MCR);
static volatile uint32_t *const ka200_uart_lsr =
	(volatile uint32_t *)(KA200_UART0_BASE + KA200_UART_LSR);

static void ka200_uart_early_init(void)
{
	uint32_t ibrd;
	uint32_t lcr;

	*ka200_uart_lcr = 0x3U;
	*ka200_uart_ier = (*ka200_uart_ier & 0x40U);
	*ka200_uart_iir = 0U;
	*ka200_uart_mcr = 0x3U;

	ibrd = ((KA200_UART_REF_CLK_HZ / KA200_UART_BAUD) * 1000U / 16U) / 1000U;
	lcr = *ka200_uart_lcr;
	*ka200_uart_lcr = lcr | KA200_UART_LCR_DLAB;
	*ka200_uart_thr = ibrd & 0xffU;
	*ka200_uart_ier = (ibrd >> 8) & 0xffU;
	*ka200_uart_lcr = lcr & ~KA200_UART_LCR_DLAB;
}

static void ka200_uart_putc(char c)
{
	while ((*ka200_uart_lsr & KA200_UART_LSR_THRE) == 0U) {
		;
	}

	*ka200_uart_thr = (uint32_t)c;
}

void ka200_boot_marker(char tag)
{
	ka200_uart_putc(tag);
}

static void ka200_boot_stage(const char *msg)
{
	while (*msg != '\0') {
		ka200_uart_putc(*msg++);
	}
	ka200_uart_putc('\r');
	ka200_uart_putc('\n');
}

static unsigned int ka200_read_el(void)
{
	uint64_t reg;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(reg));
	return (unsigned int)((reg >> 2) & 3U);
}

void ka200_prep_trace(char step)
{
	ka200_boot_marker(step);
}

void ka200_boot_debug_highest(void)
{
	static bool uart_ready;

	if (!uart_ready) {
		ka200_uart_early_init();
		uart_ready = true;
		ka200_boot_stage("Zephyr SPL");
	}

	ka200_boot_marker((char)('0' + ka200_read_el()));
}
