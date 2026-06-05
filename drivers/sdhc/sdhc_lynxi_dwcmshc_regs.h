/*
 * Copyright (c) 2026 Lynxi Technologies Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDHCI / DWC MSHC register definitions — from RT-Thread drv_sdhci.h
 */

#ifndef ZEPHYR_DRIVERS_SDHC_SDHC_LYNXI_DWCMSHC_REGS_H_
#define ZEPHYR_DRIVERS_SDHC_SDHC_LYNXI_DWCMSHC_REGS_H_

#include <stdint.h>

#define SDHCI_DMA_ADDRESS              0x00
#define SDHCI_BLOCK_SIZE               0x04
#define SDHCI_BLOCK_COUNT32            0x00
#define SDHCI_ARGUMENT                 0x08
#define SDHCI_TRANSFER_MODE            0x0C
#define SDHCI_TRNS_DMA                 0x01
#define SDHCI_TRNS_BLK_CNT_EN          0x02
#define SDHCI_TRNS_AUTO_CMD12          0x04
#define SDHCI_TRNS_AUTO_CMD23          0x08
#define SDHCI_TRNS_READ                0x10
#define SDHCI_TRNS_MULTI               0x20
#define SDHCI_COMMAND                  0x0E
#define SDHCI_CMD_RESP_NONE            0x00
#define SDHCI_CMD_RESP_LONG            0x01
#define SDHCI_CMD_RESP_SHORT           0x02
#define SDHCI_CMD_RESP_SHORT_BUSY      0x03
#define SDHCI_CMD_CRC                  0x08
#define SDHCI_CMD_INDEX                0x10
#define SDHCI_CMD_DATA                 0x20
#define SDHCI_MAKE_CMD(c, f)           (((c & 0xff) << 8) | (f & 0xff))
#define SDHCI_MAKE_BLKSZ(dma, blksz)   (((dma & 0x7) << 12) | (blksz & 0xFFF))
#define SDHCI_RESPONSE                 0x10
#define SDHCI_BUFFER                   0x20
#define SDHCI_PRESENT_STATE            0x24
#define SDHCI_CMD_INHIBIT              0x00000001
#define SDHCI_DATA_INHIBIT             0x00000002
#define SDHCI_DATA_AVAILABLE           0x00000800
#define SDHCI_SPACE_AVAILABLE          0x00000400
#define SDHCI_DAT0_SIGNAL_LEVEL        0x01000000
#define SDHCI_HOST_CONTROL             0x28
#define SDHCI_CTRL_4BITBUS             0x02
#define SDHCI_CTRL_HISPD               0x04
#define SDHCI_CTRL_DMA_MASK            0x18
#define SDHCI_CTRL_8BITBUS             0x20
#define SDHCI_POWER_CONTROL            0x29
#define SDHCI_POWER_ON                 0x01
#define SDHCI_POWER_180                0x0A
#define SDHCI_POWER_330                0x0E
#define SDHCI_CLOCK_CONTROL            0x2C
#define SDHCI_DIVIDER_SHIFT            8
#define SDHCI_DIV_MASK                 0xFF
#define SDHCI_DIV_HI_MASK              0x300
#define SDHCI_DIV_MASK_LEN             8
#define SDHCI_PROG_CLOCK_MODE          0x0020
#define SDHCI_CLOCK_CARD_EN            0x0004
#define SDHCI_CLOCK_PLL_EN             0x0008
#define SDHCI_CLOCK_INT_STABLE         0x0002
#define SDHCI_CLOCK_INT_EN             0x0001
#define SDHCI_TIMEOUT_CONTROL          0x2E
#define SDHCI_SOFTWARE_RESET           0x2F
#define SDHCI_RESET_ALL                0x01
#define SDHCI_RESET_CMD                0x02
#define SDHCI_RESET_DATA               0x04
#define SDHCI_INT_STATUS               0x30
#define SDHCI_INT_ENABLE               0x34
#define SDHCI_SIGNAL_ENABLE            0x38
#define SDHCI_INT_RESPONSE             0x00000001
#define SDHCI_INT_DATA_END             0x00000002
#define SDHCI_INT_SPACE_AVAIL          0x00000010
#define SDHCI_INT_DATA_AVAIL           0x00000020
#define SDHCI_INT_ERROR                0x00008000
#define SDHCI_INT_TIMEOUT              0x00010000
#define SDHCI_INT_CRC                  0x00020000
#define SDHCI_INT_END_BIT              0x00040000
#define SDHCI_INT_INDEX                0x00080000
#define SDHCI_INT_CMD_ERR_MASK         (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | \
					SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define SDHCI_INT_CMD_MASK             (SDHCI_INT_RESPONSE | SDHCI_INT_CMD_ERR_MASK)
/* HE200 DWC: CMD TIMEOUT irq (0x10000) precedes CC; do not enable it */
#define LYNXI_INT_CMD_ENABLE           (SDHCI_INT_CMD_MASK & ~SDHCI_INT_TIMEOUT)
#define SDHCI_INT_DATA_MASK            (SDHCI_INT_DATA_END | 0x00F00000)
#define SDHCI_HOST_CONTROL2            0x3E
#define SDHCI_CTRL_UHS_MASK            0x0007
#define SDHCI_CTRL_UHS_SDR12           0x0000
#define SDHCI_CTRL_UHS_SDR25           0x0001
#define SDHCI_CTRL_UHS_SDR50           0x0002
#define SDHCI_CTRL_UHS_SDR104          0x0003
#define SDHCI_CTRL_UHS_DDR50           0x0004
#define SDHCI_CTRL_HS400               0x0005
#define SDHCI_CTRL_VDD_180             0x0008
#define SDHCI_CMD23_ENABLE             0x0800
#define SDHCI_CTRL_V4_MODE             0x1000
#define SDHCI_CTRL_64BIT_ADDR          0x2000
#define SDHCI_MAX_DIV_SPEC_300         2046
#define SDHCI_INT_ALL_MASK             ((uint32_t)-1)
#define SDHCI_HOST_VERSION             0xfc
#define SDHCI_CAPABILITIES             0x40
#define SDHCI_CAPABILITIES1            0x44

#define DWC_MSHC_PTR_VENDOR1           0x500
#define SDHCI_VENDER_AT_CTRL_REG       (DWC_MSHC_PTR_VENDOR1 + 0x40)
#define SDHCI_VENDER_AT_STAT_REG       (DWC_MSHC_PTR_VENDOR1 + 0x44)
#define DWC_MSHC_PTR_PHY_REGS          0x300
#define MSHC_CTRL_R                    (DWC_MSHC_PTR_VENDOR1 + 0x08)
#define EMMC_CTRL_R                    (DWC_MSHC_PTR_VENDOR1 + 0x2c)
#define CARD_IS_EMMC                   0
#define EMMC_RST_N                     2
#define EMMC_RST_N_OE                  3
#define DWC_MSHC_PHY_CNFG              (DWC_MSHC_PTR_PHY_REGS + 0x0)
#define PAD_SN_LSB                     20
#define PAD_SN_MASK                    0xF
#define PAD_SP_LSB                     16
#define PAD_SP_MASK                    0xF
#define PAD_SN_DEFAULT                 ((0x9 & PAD_SN_MASK) << PAD_SN_LSB)
#define PAD_SP_DEFAULT                 ((0x9 & PAD_SP_MASK) << PAD_SP_LSB)
#define PHY_PWRGOOD                    BIT(1)
#define PHY_RSTN                       BIT(0)
#define DWC_MSHC_CMDPAD_CNFG           (DWC_MSHC_PTR_PHY_REGS + 0x4)
#define DWC_MSHC_DATPAD_CNFG           (DWC_MSHC_PTR_PHY_REGS + 0x6)
#define DWC_MSHC_CLKPAD_CNFG           (DWC_MSHC_PTR_PHY_REGS + 0x8)
#define DWC_MSHC_STBPAD_CNFG           (DWC_MSHC_PTR_PHY_REGS + 0xA)
#define DWC_MSHC_RSTNPAD_CNFG          (DWC_MSHC_PTR_PHY_REGS + 0xC)
#define TXSLEW_N_LSB                   9
#define TXSLEW_P_LSB                   5
#define WEAKPULL_EN_LSB                3
#define RXSEL_LSB                      0
#define DWC_MSHC_SDCLKDL_CNFG          (DWC_MSHC_PTR_PHY_REGS + 0x1D)
#define DWC_MSHC_SDCLKDL_DC            (DWC_MSHC_PTR_PHY_REGS + 0x1E)
#define DWC_MSHC_SMPLDL_CNFG           (DWC_MSHC_PTR_PHY_REGS + 0x20)
#define DWC_MSHC_ATDL_CNFG             (DWC_MSHC_PTR_PHY_REGS + 0x21)
#define DWC_MSHC_PHY_PAD_EMMC_CLK      ((2 << TXSLEW_N_LSB) | (2 << TXSLEW_P_LSB) | (1 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_EMMC_DAT      ((3 << TXSLEW_N_LSB) | (3 << TXSLEW_P_LSB) | (1 << WEAKPULL_EN_LSB) | (1 << RXSEL_LSB))

#endif /* ZEPHYR_DRIVERS_SDHC_SDHC_LYNXI_DWCMSHC_REGS_H_ */
