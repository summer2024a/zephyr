/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Earliest SPL→Zephyr hook: no stack, no U-Boot UART setup.
 * UART0 @ 0x10006000, 50MHz ref clk, 115200 8n1 (same as RT-Thread he200 BSP).
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

/* void he200_ep_reset_hook(void);  preserve x23/x24/x25 */
GTEXT(he200_ep_reset_hook)
SECTION_FUNC(TEXT, he200_ep_reset_hook)
	mov	x9, x30			/* save lr */

	movz	x19, #0x6000
	movk	x19, #0x1000, lsl #16	/* x19 = 0x10006000 */

	/* LCR = 8n1 */
	mov	w0, #3
	str	w0, [x19, #0xc]

	/* IER: keep only bit 6 */
	ldr	w0, [x19, #4]
	and	w0, w0, #0x40
	str	w0, [x19, #4]

	/* disable FIFO */
	str	wzr, [x19, #8]

	/* MCR: DTR + RTS */
	mov	w0, #3
	str	w0, [x19, #0x10]

	/* divisor: (50000000/115200*1000/16)/1000 = 27 */
	ldr	w0, [x19, #0xc]
	orr	w0, w0, #0x80			/* DLAB */
	str	w0, [x19, #0xc]
	mov	w0, #27
	str	w0, [x19, #0]			/* DLL */
	str	wzr, [x19, #4]			/* DLM */
	ldr	w0, [x19, #0xc]
	bic	w0, w0, #0x80
	str	w0, [x19, #0xc]

	/* 'S' = entered __start from image header */
1:	ldr	w1, [x19, #0x14]
	tst	w1, #0x20
	b.eq	1b
	mov	w1, #'S'
	str	w1, [x19]

	mov	x30, x9
	ret
