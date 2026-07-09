# SPDX-License-Identifier: Apache-2.0

zephyr_library_sources_ifdef(CONFIG_ARM_MMU mmu_regions.c)
zephyr_library_sources_ifdef(CONFIG_SOC_AMLOGIC_MESON_S4
	meson_s4_plat.c
	meson_s4_early_uart.c
)
zephyr_library_sources_ifdef(CONFIG_SMP meson_s4_smp_boot.c)
zephyr_library_sources_ifdef(CONFIG_SOC_MESON_S4_BOOT_TRACE
	meson_s4_boot_debug.S
)
zephyr_library_sources_ifdef(CONFIG_SOC_MESON_S4_SHELL_SMP
	meson_s4_smp_shell.c
	meson_s4_irq_shell.c
)
zephyr_library_sources_ifdef(CONFIG_SOC_MESON_S4_SMP_AUTO_PROBE meson_s4_smp_probe.c)
zephyr_library_sources_ifdef(CONFIG_SOC_MESON_S4_SMP_DEFER_INIT meson_s4_smp_defer.c)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm64/scripts/linker.ld CACHE INTERNAL "")