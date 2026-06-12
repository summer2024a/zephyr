# SPDX-License-Identifier: Apache-2.0

set(BOARD_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld CACHE INTERNAL "Board linker script, s905y4_2g")

# SoC bring-up (early UART, SMP shell) lives in soc/amlogic/s4/

# Post-build: create uImage for U-Boot bootm deployment.
# Load address = Run address = 0x01000000 (NS_BL33_ENTRYPOINT for S4 BL31).
# See BOOT.md for packaging details.
if(CONFIG_BOARD_S905Y4_2G AND CONFIG_BUILD_OUTPUT_BIN)
  find_program(MKIMAGE mkimage)

  if(NOT ${MKIMAGE} STREQUAL MKIMAGE-NOTFOUND)
    set(UIMAGE_NAME ${CONFIG_KERNEL_BIN_NAME}.uimg)
    set(UIMAGE_PATH ${PROJECT_BINARY_DIR}/${UIMAGE_NAME})

    set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
      COMMAND ${MKIMAGE}
      ARGS -A arm64 -O u-boot -T standalone -C none
           -a 0x01000000 -e 0x01000000
           -n "Zephyr S4 S905Y4"
           -d ${CONFIG_KERNEL_BIN_NAME}.bin
           ${UIMAGE_NAME}
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    )
    message(STATUS "uImage will be written to: ${UIMAGE_PATH}")
  else()
    message(STATUS "mkimage not found; uImage will not be generated automatically.")
    message(STATUS "Install u-boot-tools or run post_build_mkimage.sh manually.")
  endif()
endif()