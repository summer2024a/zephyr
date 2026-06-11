#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Post-build script to create uImage for Amlogic S4 using mkimage.
# Load address = Run address = 0x01000000 (NS_BL33_ENTRYPOINT).
#
# Usage: post_build_mkimage.sh <zephyr.bin_path>
#
# The Zephyr CMakeLists.txt for s905y4_2g board already includes a
# post-build mkimage step. This script can be used standalone for
# manual packaging or debugging.

set -e

BIN_FILE="${1:-zephyr.bin}"
LOAD_ADDR=0x01000000
ENTRY_ADDR=0x01000000
UIMG_FILE="${BIN_FILE%.bin}.uimg"

if [ ! -f "$BIN_FILE" ]; then
    echo "Error: Binary file '$BIN_FILE' not found"
    exit 1
fi

# Check if mkimage is available
if ! command -v mkimage &>/dev/null; then
    echo "Error: mkimage not found. Install u-boot-tools package."
    echo "  Ubuntu/Debian: sudo apt install u-boot-tools"
    exit 1
fi

echo "Packaging $BIN_FILE -> $UIMG_FILE"
echo "  Load address:  $LOAD_ADDR"
echo "  Entry address:  $ENTRY_ADDR"

mkimage -A arm64 -O u-boot -T standalone -C none \
    -a "$LOAD_ADDR" -e "$ENTRY_ADDR" \
    -n "Zephyr S4 S905Y4" \
    -d "$BIN_FILE" "$UIMG_FILE"

echo "Done. uImage created: $UIMG_FILE"
echo "  Size: $(stat -c%s "$UIMG_FILE") bytes"
echo ""
echo "Deploy to U-Boot:"
echo "  fatload mmc 1 0x01000000 zephyr.uimg"
echo "  bootm 0x01000000"