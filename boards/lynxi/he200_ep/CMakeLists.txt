# SPDX-License-Identifier: Apache-2.0

set(BOARD_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld CACHE INTERNAL "Board linker script, he200_ep")

# SoC bring-up (SPL, GIC EL2, early UART, ka200_smp) lives in soc/lynxi/ka200/
