# SPDX-License-Identifier: Apache-2.0

set(BOARD_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld CACHE INTERNAL "Board linker script, s905y4_2g")

# SoC bring-up (early UART, SMP shell) lives in soc/amlogic/s4/