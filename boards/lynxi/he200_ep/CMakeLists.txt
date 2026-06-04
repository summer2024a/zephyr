# SPDX-License-Identifier: Apache-2.0

set(BOARD_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld CACHE INTERNAL "Board linker script, he200_ep")

# 平台特定初始化代码
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_GIC_V3
    ${CMAKE_CURRENT_SOURCE_DIR}/he200_ep_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/he200_ep_spl.c
)

zephyr_library_sources_ifdef(CONFIG_HE200_EP_EARLY_UART_DEBUG
    ${CMAKE_CURRENT_SOURCE_DIR}/he200_ep_early_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/he200_ep_boot_debug.S
)
