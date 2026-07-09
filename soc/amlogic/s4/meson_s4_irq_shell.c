/*
 * Copyright (c) 2026 Amlogic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * S4 GIC / UART_B interrupt diagnostics shell command.
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/interrupt_controller/gic.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_io.h>

#define S4_UART_B_PHYS  0xFE07A000U
#define S4_UART_Z_IRQ   DT_IRQN(DT_NODELABEL(uart_b))
#define S4_UART_HW_IRQ  169U

#define UART_CONTROL  0x08U
#define UART_STATUS   0x0CU
#define UART_MISC     0x10U

#define RX_EMPTY  BIT(20)
#define TX_FULL   BIT(21)
#define RX_INT_EN BIT(27)
#define TX_INT_EN BIT(28)

#ifdef CONFIG_SOC_MESON_S4_SHELL_SMP
extern uint32_t meson_uart_isr_count;
#endif

static uint32_t gicd_bit(unsigned int irq, mem_addr_t reg)
{
	unsigned int grp = irq / 32U;
	unsigned int off = irq % 32U;
	uint32_t val = sys_read32(reg + grp * 4U);

	return (val >> off) & 0x1U;
}

static uint32_t gicd_icfgr_bits(unsigned int irq)
{
	unsigned int grp = (irq / 16U) * 4U;
	unsigned int off = (irq % 16U) * 2U;
	uint32_t val = sys_read32(GICD_ICFGRn + grp);

	return (val >> off) & 0x3U;
}

static uint32_t gicd_ipriority(unsigned int irq)
{
	return sys_read8(GICD_IPRIORITYRn + irq);
}

static uint32_t gicd_itarget(unsigned int irq)
{
	return sys_read8(GICD_ITARGETSRn + irq);
}

static const char *gic_icfgr_name(uint32_t cfg)
{
	switch (cfg & 0x3U) {
	case 0U: return "level-low";
	case 1U: return "level-high";
	case 2U: return "edge-rising";
	case 3U: return "edge-falling";
	default: return "?";
	}
}

static int cmd_meson_s4_gic(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int zirq = S4_UART_Z_IRQ;
	unsigned int hwirq = S4_UART_HW_IRQ;
	uint32_t gicc = sys_read32(GICC_CTLR);
	uint32_t ctrl = sys_read32(S4_UART_B_PHYS + UART_CONTROL);
	uint32_t status = sys_read32(S4_UART_B_PHYS + UART_STATUS);
	uint32_t misc = sys_read32(S4_UART_B_PHYS + UART_MISC);
	uint32_t icfgr = gicd_icfgr_bits(zirq);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "UART_B @ 0x%08x hwirq=%u zephyr_irq=%u",
		    S4_UART_B_PHYS, hwirq, zirq);
	shell_print(sh, "  CONTROL=0x%08x STATUS=0x%08x MISC=0x%08x",
		    ctrl, status, misc);
	shell_print(sh, "  TX/RX INT_EN=%u/%u RX_EMPTY=%u TX_FULL=%u",
		    (ctrl & TX_INT_EN) ? 1U : 0U, (ctrl & RX_INT_EN) ? 1U : 0U,
		    (status & RX_EMPTY) ? 1U : 0U, (status & TX_FULL) ? 1U : 0U);

	shell_print(sh, "GIC (zephyr irq %u = Linux SPI %u):", zirq, hwirq);
	shell_print(sh, "  enabled=%u pending=%u active=%u",
		    gicd_bit(zirq, GICD_ISENABLERn),
		    gicd_bit(zirq, GICD_ISPENDRn),
		    gicd_bit(zirq, GICD_ISACTIVERn));
	shell_print(sh, "  ICFGR=0x%x (%s) IGROUP=%u target=0x%02x prio=0x%02x",
		    icfgr, gic_icfgr_name(icfgr),
		    gicd_bit(zirq, GICD_IGROUPRn),
		    gicd_itarget(zirq), gicd_ipriority(zirq));

	shell_print(sh, "GIC CPU IF:");
	shell_print(sh, "  GICC_CTLR=0x%08x PMR=0x%02x arch_irq_is_enabled=%d",
		    gicc, sys_read32(GICC_PMR) & 0xffU, arch_irq_is_enabled(zirq));

	shell_print(sh, "Arch timer PPI 27: enabled=%u pending=%u",
		    gicd_bit(27U, GICD_ISENABLERn),
		    gicd_bit(27U, GICD_ISPENDRn));

#ifdef CONFIG_SOC_MESON_S4_SHELL_SMP
	shell_print(sh, "meson_uart_isr_count=%u", meson_uart_isr_count);
#endif

	return 0;
}

SHELL_CMD_REGISTER(meson_s4_gic, NULL, "S4 GIC/UART_B IRQ diagnostics", cmd_meson_s4_gic);
SHELL_CMD_REGISTER(s4_gic, NULL, "Alias of meson_s4_gic", cmd_meson_s4_gic);
