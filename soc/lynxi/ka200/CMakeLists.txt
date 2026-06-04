# SPDX-License-Identifier: Apache-2.0

zephyr_library_sources_ifdef(CONFIG_ARM_MMU mmu_regions.c)
zephyr_library_sources_ifdef(CONFIG_SOC_LYNXI_KA200_SPIN_TABLE pm_cpu_ops_spin_table.c)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm64/scripts/linker.ld CACHE INTERNAL "")