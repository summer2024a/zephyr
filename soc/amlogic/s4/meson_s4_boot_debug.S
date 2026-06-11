/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Earliest boot debug for Amlogic S4: UART_B @ 0xFE07A000.
 * Meson UART register layout (see meson_s4_early_uart.c for details).
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(meson_s4_reset_hook)
SECTION_FUNC(TEXT, meson_s4_reset_hook)
	mov	x9, x30

	/* UART_B base address: 0xFE07A000 */
	movz	x19, #0xa000
	movk	x19, #0xfe07, lsl #16

	/* CONTROL register offset 0x08 */
	/* Reset TX/RX FIFO: set TX_RST (bit22) + RX_RST (bit23) + CLR_ERR (bit24) */
	ldr	w0, [x19, #0x08]
	orr	w0, w0, #(1 << 22) | (1 << 23) | (1 << 24)
	str	w0, [x19, #0x08]

	/* Clear reset bits (auto-clear, but explicit) */
	bic	w0, w0, #(1 << 22) | (1 << 23) | (1 << 24)
	str	w0, [x19, #0x08]

	/* Enable TX_EN (bit12) + RX_EN (bit13) */
	orr	w0, w0, #(1 << 12) | (1 << 13)
	str	w0, [x19, #0x08]

	/* Print 'S' marker: wait TX_FULL clear then write */
1:	ldr	w1, [x19, #0x0c]	/* STATUS register */
	tst	w1, #(1 << 21)		/* TX_FULL bit */
	b.ne	1b
	mov	w1, #'S'
	str	w1, [x19]		/* WFIFO */

	mov	x30, x9
	ret