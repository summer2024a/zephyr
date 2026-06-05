/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Earliest SPL→Zephyr hook: UART0 @ 0x10006000, 50MHz, 115200.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(ka200_reset_hook)
SECTION_FUNC(TEXT, ka200_reset_hook)
	mov	x9, x30

	movz	x19, #0x6000
	movk	x19, #0x1000, lsl #16

	mov	w0, #3
	str	w0, [x19, #0xc]

	ldr	w0, [x19, #4]
	and	w0, w0, #0x40
	str	w0, [x19, #4]

	str	wzr, [x19, #8]

	mov	w0, #3
	str	w0, [x19, #0x10]

	ldr	w0, [x19, #0xc]
	orr	w0, w0, #0x80
	str	w0, [x19, #0xc]
	mov	w0, #27
	str	w0, [x19, #0]
	str	wzr, [x19, #4]
	ldr	w0, [x19, #0xc]
	bic	w0, w0, #0x80
	str	w0, [x19, #0xc]

1:	ldr	w1, [x19, #0x14]
	tst	w1, #0x20
	b.eq	1b
	mov	w1, #'S'
	str	w1, [x19]

	mov	x30, x9
	ret
