/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Amlogic S4 early UART_B output for boot trace markers.
 * Meson UART register layout (not ns16550 compatible).
 *
 * UART_B base: 0xFE07A000 (from meson-s4.dtsi serial@7a000)
 *   WFIFO   +0x00  — write character
 *   RFIFO   +0x04  — read received character
 *   CONTROL +0x08  — control register
 *   STATUS  +0x0C  — status register
 *   MISC    +0x10
 *   REG5    +0x14  — new baud rate control
 *
 * STATUS bits:
 *   bit 20: RX_EMPTY
 *   bit 21: TX_FULL  (wait until 0 before writing)
 *   bit 22: TX_EMPTY
 *   bit 25: XMIT_BUSY
 *
 * CONTROL bits:
 *   bit 12: TX_EN
 *   bit 13: RX_EN
 *   bit 22: TX_RST  (auto-clears)
 *   bit 23: RX_RST  (auto-clears)
 *   bit 24: CLR_ERR (auto-clears)
 *
 * Clock: CLKID_UART_B = 187, enable bit 27 in offset 0x14 of clkc_periphs (0xFE000000)
 */

#include <stdbool.h>
#include <stdint.h>

#define S4_UART_B_BASE           0xFE07A000U
#define S4_CLKC_PERIPHS_BASE     0xFE000000U

#define UART_WFIFO               0x00U
#define UART_RFIFO               0x04U
#define UART_CONTROL             0x08U
#define UART_STATUS              0x0CU
#define UART_MISC                0x10U
#define UART_REG5                0x14U

/* CONTROL register bits */
#define TX_EN_BIT                (1U << 12)
#define RX_EN_BIT                (1U << 13)
#define TX_RST_BIT               (1U << 22)
#define RX_RST_BIT               (1U << 23)
#define CLR_ERR_BIT              (1U << 24)
#define UART_INIT_MASK           (TX_RST_BIT | RX_RST_BIT | CLR_ERR_BIT | TX_EN_BIT | RX_EN_BIT)

/* STATUS register bits */
#define TX_FULL_BIT              (1U << 21)
#define TX_EMPTY_BIT             (1U << 22)

/* Clock enable: CLKID_UART_B=187, bit 27 in offset 0x14 */
#define CLK_EN_OFFSET            0x14U
#define CLK_EN_BIT               (1U << 27)

static volatile uint32_t *const s4_uart_wfifo =
	(volatile uint32_t *)(S4_UART_B_BASE + UART_WFIFO);
static volatile uint32_t *const s4_uart_control =
	(volatile uint32_t *)(S4_UART_B_BASE + UART_CONTROL);
static volatile uint32_t *const s4_uart_status =
	(volatile uint32_t *)(S4_UART_B_BASE + UART_STATUS);

static bool uart_initialized;

static void s4_uart_early_init(void)
{
	uint32_t ctrl;

	/* Reset TX/RX FIFOs, clear errors, then enable TX/RX */
	ctrl = *s4_uart_control;
	ctrl |= UART_INIT_MASK;
	*s4_uart_control = ctrl;

	/* Pulse bits auto-clear; clear reset/error bits explicitly */
	ctrl &= ~(TX_RST_BIT | RX_RST_BIT | CLR_ERR_BIT);
	*s4_uart_control = ctrl;

	/* Enable TX and RX */
	ctrl |= (TX_EN_BIT | RX_EN_BIT);
	*s4_uart_control = ctrl;

	/* Wait for TX to be ready — polls until UART responds.
	 * This handles the case where UART clock may not be fully
	 * stabilized yet (e.g., called from EL2/EL3 before clock
	 * controller is accessible). */
	uint32_t retries = 1000;
	while ((*s4_uart_status & TX_FULL_BIT) && retries--) {
		;
	}

	uart_initialized = true;
}

static void s4_uart_putc(char c)
{
	/* Wait until TX buffer is not full */
	while ((*s4_uart_status & TX_FULL_BIT) != 0U) {
		;
	}

	/* Write character to TX FIFO */
	*s4_uart_wfifo = (uint32_t)c;

	/* Wait until TX completes (TX_EMPTY) to ensure character is sent
	 * before writing next one. This prevents UART buffer overflow issues. */
	while ((*s4_uart_status & TX_EMPTY_BIT) == 0U) {
		;
	}
}

static bool s4_boot_marker_is_cpu0(void)
{
	uint64_t mpidr;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	return (mpidr & 0xffU) == 0U;
}

void meson_s4_boot_marker(char tag)
{
	if (!s4_boot_marker_is_cpu0()) {
		return;
	}

	if (!uart_initialized) {
		s4_uart_early_init();
	}

	s4_uart_putc(tag);
}

static void s4_boot_stage(const char *msg)
{
	while (*msg != '\0') {
		s4_uart_putc(*msg++);
	}
	s4_uart_putc('\r');
	s4_uart_putc('\n');
}

void meson_s4_boot_debug_highest(void)
{
	static bool banner_printed;

	if (!uart_initialized) {
		s4_uart_early_init();
	}

	if (!banner_printed) {
		s4_boot_stage("Zephyr SPL (S4)");
		banner_printed = true;
	}

	meson_s4_boot_marker('H');
}
