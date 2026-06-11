# SPDX-License-Identifier: Apache-2.0

set(BOARD_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld CACHE INTERNAL "Board linker script, s905y4_2g")

# Post-build: create uImage via mkimage (load/run address = 0x01000000)
find_program(MKIMAGE mkimage)

if(MKIMAGE)
	add_custom_command(
		TARGET zephyr_final
		POST_BUILD
		COMMAND ${MKIMAGE}
			-A arm64
			-O u-boot
			-T standalone
			-C none
			-a 0x01000000
			-e 0x01000000
			-n "Zephyr S4 S905Y4"
			-d ${ZEPHYR_BINARY_DIR}/zephyr.bin
			${ZEPHYR_BINARY_DIR}/zephyr.uimg
		COMMENT "Creating uImage for Amlogic S4 (load=0x01000000, entry=0x01000000)"
	)
else()
	message(WARNING "mkimage not found — uImage packaging skipped. Install u-boot-tools.")
endif()