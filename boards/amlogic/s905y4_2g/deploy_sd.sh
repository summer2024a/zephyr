#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Deploy Zephyr uImage to SD card for Amlogic S4 board.
#
# Usage: deploy_sd.sh [BUILD_DIR] [SD_DEVICE]
#   BUILD_DIR  - Zephyr build directory (default: build_s4)
#   SD_DEVICE  - SD card FAT partition device (default: /dev/sdb1)
#
# Example:
#   ./deploy_sd.sh
#   ./deploy_sd.sh build_s4_shell /dev/sdc1

set -e

BUILD_DIR="${1:-build_s4}"
SD_DEV="${2:-/dev/sdb1}"
MNT="/mnt/sdcard"
BIN_FILE="${BUILD_DIR}/zephyr/zephyr.bin"
UIMG_FILE="${BUILD_DIR}/zephyr/zephyr.uimg"
LOAD_ADDR=0x01000000
ENTRY_ADDR=0x01000000

# Check if binary exists
if [ ! -f "$BIN_FILE" ]; then
    echo "Error: Binary not found at $BIN_FILE"
    echo "Run 'west build -b s905y4_2g -d $BUILD_DIR' first"
    exit 1
fi

# Create uImage if not exists
if [ ! -f "$UIMG_FILE" ]; then
    echo "Creating uImage..."
    mkimage -A arm64 -O u-boot -T standalone -C none \
        -a "$LOAD_ADDR" -e "$ENTRY_ADDR" \
        -n "Zephyr S4 S905Y4" \
        -d "$BIN_FILE" "$UIMG_FILE"
    echo "uImage created: $UIMG_FILE ($(stat -c%s "$UIMG_FILE") bytes)"
fi

# Verify uImage header
echo "Verifying uImage..."
mkimage -l "$UIMG_FILE"

# Check SD card device
if [ ! -b "$SD_DEV" ]; then
    echo "Error: SD card device $SD_DEV not found"
    echo "Available devices:"
    lsblk -d -o NAME,SIZE,TYPE | grep disk
    echo ""
    echo "Specify the correct partition device, e.g.:"
    echo "  $0 $BUILD_DIR /dev/sdX1"
    exit 1
fi

# Mount and copy
echo "Mounting $SD_DEV..."
mkdir -p "$MNT"
mount "$SD_DEV" "$MNT"

echo "Copying uImage to SD card..."
cp "$UIMG_FILE" "$MNT/"
sync
umount "$MNT"

echo "======================================"
echo "  Deployment complete!"
echo "======================================"
echo ""
echo "Insert SD card into S4 board."
echo "In U-Boot serial console:"
echo ""
echo "  fatload mmc 1 0x01000000 zephyr.uimg"
echo "  bootm 0x01000000"
echo ""
echo "Serial console: UART_B, 115200, 8N1"
echo "======================================"