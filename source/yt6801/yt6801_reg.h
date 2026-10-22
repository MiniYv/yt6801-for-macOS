/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * yt6801_reg.h — Motorcomm YT6801 Gigabit Ethernet controller registers
 *
 * Values reflect the real chip; derived from the vendor Linux driver
 * ("fuxi-gmac" v1.0.30, Copyright (c) 2021 Motor-comm Corporation,
 * GPL-2.0-or-later). See the NOTICE file at the repository root.
 *
 * All addresses/bitfields are real chip values, no placeholders. Only
 * what macOS/DriverKit needs is kept: register addresses, bitfields,
 * descriptor layout, PHY timing constants.
 */
#ifndef YT6801_REG_H
#define YT6801_REG_H

/* ==================== Device identity ==================== */
#define YT6801_DRV_NAME                 "yt6801"
#ifndef YT6801_DRV_VERSION
#define YT6801_DRV_VERSION              "0.0.0"  /* build.sh injects the plist version */
#endif
#define YT6801_VENDOR_ID                0x1F0A  /* Motorcomm */
#define YT6801_DEVICE_ID                0x6801
/* PHY ID in the FIFO section below (0x4F51:0xE91A, YT8614) */

/* ==================================================================
 * MAC register block offset within BAR0.
 * All addresses below (MAC_CR 0x0000, DMA_MR 0x1000, MTL 0x0d00,
 * DMA_CH 0x1100 ...) are offsets inside the MAC block; the real MMIO
 * address is BAR0 + 0x2000 + offset.
 * ================================================================== */
#define YT6801_MAC_REGS_OFFSET          0x2000

/* ==================================================================
 * MGMT block — directly relative to BAR0, no +0x2000.
 * PHY hardware reset/release control.
 * ================================================================== */
#define MGMT_EPHY_CTRL                  0x1004
#define MGMT_EPHY_CTRL_RST_POS          0       /* 0=held in reset, 1=released */
#define MGMT_EPHY_CTRL_LINKUP_POS       1       /* read-only status */
#define MGMT_EPHY_CTRL_SPEED_POS        3       /* read-only status */

/* ==================================================================
 * EFUSE controller — bare BAR0 address, no +0x2000 (same as MGMT).
 * ================================================================== */
#define EFUSE_OP_CTRL_0                 0x1500  /* command register */
#define EFUSE_OP_ADDR_POS               8       /* bit15:8  row address */
#define EFUSE_OP_ADDR_LEN               8
#define EFUSE_OP_START_POS              2       /* bit2    start */
#define EFUSE_OP_MODE_POS               0       /* bit1:0  mode */
#define EFUSE_OP_MODE_LEN               2
#define EFUSE_OP_MODE_ROW_READ          0x1     /* row read */
#define EFUSE_OP_CTRL_1                 0x1504  /* status/data register */
#define EFUSE_OP_RD_DATA_POS            24      /* bit31:24 read byte */
#define EFUSE_OP_DONE_POS               1       /* bit1    done flag */

/* Patch table layout: each entry is 6 bytes = 2-byte register id +
 * 4-byte value, starting at EFUSE row 18; id 0x0000 marks the end. */
#define EFUSE_PATCH_ADDR_START_BYTE     0
#define EFUSE_PATCH_DATA_START_BYTE     2
#define EFUSE_REGION_A_B_LENGTH         18
#define EFUSE_EACH_PATH_SIZE            6
#define EFUSE_MAX_ENTRY                 39
#define EFUSE_MAC_LR_ID                 0x1520  /* MAC address low 32-bit entry id */
#define EFUSE_MAC_HR_ID                 0x1524  /* MAC address high 16-bit entry id */

/* ==================== MAC registers (relative to MAC block base) ==================== */
#define MAC_CR                          0x0000  /* configuration */
#define MAC_ECR                         0x0004  /* extended configuration */
#define MAC_PFR                         0x0008  /* packet filtering */
#define MAC_WTR                         0x000C  /* watchdog timeout */
#define MAC_HTR0                        0x0010  /* hash table */
#define MAC_VLANTR                      0x0050
#define MAC_VLANHTR                     0x0058
#define MAC_VLANIR                      0x0060
#define MAC_IVLANIR                     0x0064
#define MAC_Q0TFCR                      0x0070  /* queue 0 TX flow control */
#define MAC_RFCR                        0x0090  /* RX flow control */
#define MAC_RQC0R                       0x00A0  /* RX queue control 0 */
#define MAC_RQC1R                       0x00A4
#define MAC_RQC2R                       0x00A8
#define MAC_RQC3R                       0x00AC
#define MAC_ISR                         0x00B0  /* interrupt status */
#define MAC_IER                         0x00B4  /* interrupt enable */
#define MAC_TX_RX_STA                   0x00B8  /* TX/RX status */
#define MAC_PMT_STA                     0x00C0  /* power management status */
#define MAC_RWK_PAC                     0x00C4
#define MAC_LPI_STA                     0x00D0
#define MAC_LPI_CONTROL                 0x00D4
#define MAC_LPI_TIMER                   0x00D8
#define MAC_AN_CR                       0x00E0  /* auto-negotiation control */
#define MAC_AN_SR                       0x00E4
#define MAC_AN_ADV                      0x00E8
#define MAC_AN_LPA                      0x00EC
#define MAC_AN_EXP                      0x00F0
#define MAC_PHYIF_STA                   0x00F8
#define MAC_VR                          0x0110  /* version */
#define MAC_DBG_STA                     0x0114
#define MAC_HWF0R                       0x011C  /* hardware features 0 */
#define MAC_HWF1R                       0x0120
#define MAC_HWF2R                       0x0124
#define MAC_HWF3R                       0x0128
#define MAC_MDIO_ADDRESS                0x0200
#define MAC_MDIO_DATA                   0x0204
#define MAC_GPIOCR                      0x0208
#define MAC_GPIO_SR                     0x020C
#define MAC_ARP_PROTO_ADDR              0x0210
#define MAC_CSR_SW_CTRL                 0x0230  /* software reset control */
#define MAC_MACA0HR                     0x0300  /* MAC address high 16 bits */
#define MAC_MACA0LR                     0x0304  /* MAC address low 32 bits */
#define MAC_MACA1HR                     0x0308
#define MAC_MACA1LR                     0x030C

#define MAC_QTFCR_INC                   4
#define MAC_MACA_INC                    4
#define MAC_HTR_INC                     4

/* MAC_CR bitfields */
#define MAC_CR_RE_POS                   0       /* receive enable */
#define MAC_CR_TE_POS                   1       /* transmit enable */
#define MAC_CR_LM_POS                   12      /* loopback mode */
#define MAC_CR_DM_POS                   13      /* duplex */
#define MAC_CR_FES_POS                  14      /* fast ethernet speed */
#define MAC_CR_PS_POS                   15      /* port select */
#define MAC_CR_JE_POS                   16      /* jumbo frame enable */
#define MAC_CR_ACS_POS                  20      /* auto pad/CRC strip */
#define MAC_CR_CST_POS                  21      /* CRC stripping */
#define MAC_CR_IPC_POS                  27      /* checksum offload */
#define MAC_CR_ARPEN_POS                31

/* MAC_RQC0R bitfields (RX queue mapping)
 *
 * Two bits per queue: RXQnEN = 0b00 off / 0b10 on. Reset value 0 leaves
 * all RX queues closed — frames are then never routed to a queue and
 * RX descriptors stay owned by hardware. */
#define MAC_RQC0R_RXQ0EN_POS            0
#define MAC_RQC0R_RXQ0EN_LEN            2
#define MAC_RXQ_ENABLED                 0x02

/* MAC_ECR bitfields */
#define MAC_ECR_DCRCC_POS               16      /* disable RX CRC checking */
#define MAC_ECR_HDSMS_POS               20      /* jumbo frame size */

/* MAC_PFR bitfields */
#define MAC_PFR_PR_POS                  0       /* promiscuous mode */
#define MAC_PFR_HUC_POS                 1       /* hash unicast */
#define MAC_PFR_HMC_POS                 2       /* hash multicast */
#define MAC_PFR_PM_POS                  4       /* pass all multicast */
#define MAC_PFR_RA_POS                  31      /* receive all */

/* MAC_ISR / MAC_IER bitfields */
#define MAC_ISR_PHYIS_POS               3
#define MAC_ISR_PMTIS_POS               4
#define MAC_ISR_LPIIS_POS               5
#define MAC_ISR_TSIS_POS                9
#define MAC_ISR_TXSTSIS_POS             13
#define MAC_ISR_RXSTSIS_POS             14

/* ==================== DMA registers ==================== */
#define DMA_MR                          0x1000  /* DMA mode */
#define DMA_SBMR                        0x1004  /* system bus mode */
#define DMA_ISR                         0x1008  /* DMA interrupt status */
#define DMA_DSR0                        0x100C  /* debug status 0 */
#define DMA_DSR1                        0x1010
#define DMA_DSR2                        0x1014
#define DMA_AXIARCR                     0x1020
#define DMA_AXIAWCR                     0x1024
#define DMA_AXIAWRCR                    0x1028
#define DMA_SAFE_ISR                    0x1080
#define DMA_ECC_IE                      0x1084
#define DMA_ECC_INT_SR                  0x1088

/* DMA_ISR bitfields */
#define DMA_ISR_MACIS_POS               17
#define DMA_ISR_MTLIS_POS               16

/* DMA_MR bitfields */
#define DMA_MR_SWR_POS                  0       /* soft reset */
#define DMA_MR_TXPR_POS                 11      /* TX priority */
#define DMA_MR_INTM_POS                 16      /* interrupt mode */
#define DMA_MR_QUREAD_POS               19

/* DMA_SBMR bitfields */
#define DMA_SBMR_FB_POS                 0       /* fixed burst */
#define DMA_SBMR_BLEN_4_POS             1
#define DMA_SBMR_BLEN_8_POS             2
#define DMA_SBMR_BLEN_16_POS            3
#define DMA_SBMR_BLEN_32_POS            4
#define DMA_SBMR_BLEN_64_POS            5
#define DMA_SBMR_BLEN_128_POS           6
#define DMA_SBMR_BLEN_256_POS           7
#define DMA_SBMR_AALE_POS               10
#define DMA_SBMR_EAME_POS               11      /* enhanced address mode enable */
#define DMA_SBMR_AAL_POS                12
#define DMA_SBMR_RD_OSR_LMT_POS         16
#define DMA_SBMR_WR_OSR_LMT_POS         24

/* DMA channel registers: first channel base 0x1100, stride 0x80 */
#define DMA_CH_BASE                     0x1100
#define DMA_CH_INC                      0x0080

#define DMA_CH_CR                       0x0000  /* channel control */
#define DMA_CH_TCR                      0x0004  /* TX control */
#define DMA_CH_RCR                      0x0008  /* RX control */
#define DMA_CH_TDLR_HI                  0x0010  /* TX ring base high 32 bits */
#define DMA_CH_TDLR_LO                  0x0014  /* TX ring base low 32 bits */
#define DMA_CH_RDLR_HI                  0x0018  /* RX ring base high 32 bits */
#define DMA_CH_RDLR_LO                  0x001C  /* RX ring base low 32 bits */
#define DMA_CH_TDTR_LO                  0x0020  /* TX tail pointer */
#define DMA_CH_RDTR_LO                  0x0028  /* RX tail pointer */
#define DMA_CH_TDRLR                    0x002C  /* TX ring length */
#define DMA_CH_RDRLR                    0x0030  /* RX ring length */
#define DMA_CH_IER                      0x0034  /* interrupt enable */
#define DMA_CH_RIWT                     0x0038  /* RX interrupt watchdog */
#define DMA_CH_CATDR_LO                 0x0044  /* current TX descriptor */
#define DMA_CH_CARDR_LO                 0x004C  /* current RX descriptor */
#define DMA_CH_CATBR_HI                 0x0050
#define DMA_CH_CATBR_LO                 0x0054
#define DMA_CH_CARBR_HI                 0x0058
#define DMA_CH_CARBR_LO                 0x005C
#define DMA_CH_SR                       0x0060  /* channel status */

/* DMA_CH_CR bitfields */
#define DMA_CH_CR_PBLX8_POS             16
#define DMA_CH_CR_SPH_POS               24      /* separate head pointer */

/* DMA_CH_TCR bitfields */
#define DMA_CH_TCR_ST_POS               0       /* start transmit */
#define DMA_CH_TCR_OSF_POS              4       /* operate on second frame */
#define DMA_CH_TCR_TSE_POS              12      /* TCP segmentation offload */
#define DMA_CH_TCR_PBL_POS              16      /* TX programmable burst length */
#define DMA_CH_TCR_PBL_LEN              6

/* DMA_CH_RCR bitfields */
#define DMA_CH_RCR_SR_POS               0       /* start receive */
#define DMA_CH_RCR_RBSZ_POS             1       /* RX buffer size */
#define DMA_CH_RCR_RBSZ_LEN             14
#define DMA_CH_RCR_PBL_POS              16      /* programmable burst length */
#define DMA_CH_RCR_PBL_LEN              6

/* DMA_CH_IER bitfields */
#define DMA_CH_IER_TIE_POS              0       /* TX interrupt enable */
#define DMA_CH_IER_TXSE_POS             1
#define DMA_CH_IER_TBUE_POS             2       /* TX buffer unavailable */
#define DMA_CH_IER_RIE_POS              6       /* RX interrupt enable */
#define DMA_CH_IER_RBUE_POS             7       /* RX buffer unavailable */
#define DMA_CH_IER_RSE_POS              8
#define DMA_CH_IER_FBEE_POS             12      /* fatal bus error */
#define DMA_CH_IER_AIE_POS              14
#define DMA_CH_IER_NIE_POS              15      /* abnormal interrupt summary */

/* DMA_CH_SR bitfields */
#define DMA_CH_SR_TI_POS                0
#define DMA_CH_SR_TPS_POS               1       /* TX process stopped */
#define DMA_CH_SR_TBU_POS               2
#define DMA_CH_SR_RI_POS                6
#define DMA_CH_SR_RBU_POS               7
#define DMA_CH_SR_RPS_POS               8       /* RX process stopped */
#define DMA_CH_SR_FBE_POS               12

/* ==================== DMA descriptors ==================== */
/*
 * Each descriptor is 16 bytes: four 32-bit words desc0..desc3.
 */
#define YT6801_DESC_SIZE                16
#define YT6801_DESC_WORDS               4

/* --- TDES3 bitfields --- */
/* FL = whole frame length, bits 0..14. Distinct from TDES2 B1L (bytes
 * moved by this descriptor); with a single buffer they are equal, but
 * FL=0 makes the hardware treat it as a zero-length frame. */
#define TDESC3_FL_POS                   0
#define TDESC3_FL_LEN                   15
#define TDESC3_FL_MASK                  0x7FFFu
#define TDESC3_TBS1_POS                 0       /* TX buffer 1 size */
#define TDESC3_TBS1_LEN                 17
#define TDESC3_CPC_POS                  26      /* CRC/pad control */
#define TDESC3_CPC_LEN                  2
#define TDESC3_LD_POS                   28      /* last descriptor */
#define TDESC3_FD_POS                   29      /* first descriptor */
#define TDESC3_CTXT_POS                 30      /* context descriptor */
#define TDESC3_OWN_POS                  31      /* ownership: 1=DMA owns */

#define TDESC3_OWN                      (1u << TDESC3_OWN_POS)
#define TDESC3_FD                       (1u << TDESC3_FD_POS)
#define TDESC3_LD                       (1u << TDESC3_LD_POS)
#define TDESC3_CPC_FULL_CRC_PAD         0x3u    /* CRC and padding by MAC */

/* --- RDESC3 bitfields --- */
#define RDESC3_PL_POS                   0       /* packet length (writeback) */
#define RDESC3_PL_LEN                   15
#define RDESC3_BUF1V_POS                24      /* buffer 1 address valid */
#define RDESC3_BUF2V_POS                25
#define RDESC3_RSV_POS                  26
#define RDESC3_LD_POS                   28
#define RDESC3_OWN_POS                  31

#define RDESC3_OWN                      (1u << RDESC3_OWN_POS)
#define RDESC3_BUF1V                    (1u << RDESC3_BUF1V_POS)
#define RDESC3_LD                       (1u << RDESC3_LD_POS)
#define RDESC3_PL_MASK                  0x7FFFu

/* --- RX writeback status bits --- */
#define RDESC3_WB_RS0V_POS              25
#define RDESC3_WB_CE_POS                24      /* CRC error */
#define RDESC3_WB_GP_POS                23      /* giant frame */
#define RDESC3_WB_RWT_POS               22      /* RX watchdog timeout */
#define RDESC3_WB_OE_POS                21      /* FIFO overflow */
#define RDESC1_WB_IPCE_POS              7       /* IP payload error */
#define RDESC1_WB_IPV4_POS              4
#define RDESC1_WB_IPV6_POS              5

/* ==================== MDIO access ==================== */
/*
 *   read:  mdioctrl = (reg << 16) | 0x800020D
 *   write: mdioctrl = (reg << 16) | 0x8000205, data written to
 *          MAC_MDIO_DATA first; then poll MAC_MDIO_ADDRESS bit0 (BUSY)
 */
#define MAC_MDIO_ADDRESS_BUSY           (1u << 0)
#define MAC_MDIO_READ_CMD               0x8000200Du
#define MAC_MDIO_WRITE_CMD              0x80002005u
#define MAC_MDIO_POLL_LIMIT             15      /* Linux busy count limit (reference) */
/* 2000 iterations x 10us = 20ms cap: enough for one 2.5MHz MDIO
 * transaction (~40us) without stalling startup when the PHY is absent. */
#define MAC_MDIO_POLL_ITERS             2000

/* ==================== PHY registers (standard MII + YT6801 private) ==================== */
#define REG_MII_BMCR                    0x00
#define REG_MII_BMSR                    0x01
#define REG_MII_PHYSID1                 0x02
#define REG_MII_PHYSID2                 0x03
#define REG_MII_ADVERTISE               0x04
#define REG_MII_LPA                     0x05
#define REG_MII_EXPANSION               0x06
#define REG_MII_CTRL1000                0x09
#define REG_MII_STAT1000                0x0A
#define REG_MII_SPEC_CTRL               0x10
#define REG_MII_SPEC_STATUS             0x11
#define REG_MII_INT_STATUS              0x13
#define REG_MII_EXT_ADDR                0x1E
#define REG_MII_EXT_DATA                0x1F

/* BMCR bits */
#define MII_BMCR_RESET                  0x8000
#define MII_BMCR_LOOPBACK               0x4000  /* IEEE 802.3 bit14: local loopback */
#define MII_BMCR_SPEED100               0x2000
#define MII_BMCR_ANENABLE               0x1000
#define MII_BMCR_ANRESTART              0x0200
#define MII_BMCR_FULLDPLX               0x0100
#define MII_BMCR_SPEED1000              0x0040

/* BMCR loopback whole-word values.
 *   0x8140 = enter loopback: bit15 soft reset loads the loopback config;
 *     the real loopback is set by ext regs 0x0A<-0x3A18 / 0x27<-0x6812,
 *     BMCR is the final step. bit15 self-clears after write.
 *   0x9140 = restore: reset + auto-neg on + 1000M full duplex. */
#define REG_MII_BMCR_ENABLE_LOOPBACK    0x8140
#define REG_MII_BMCR_DISABLE_LOOPBACK   0x9140
/* Leave loopback without a PHY soft reset (bit15 would restart auto-neg from
 * scratch); bit9 asks for an immediate re-negotiation on the live wire. */
#define REG_MII_BMCR_RESTORE_ANEG       0x1340

/* No-cable self-test: force 100M full duplex + digital loopback + AN off.
 *   0x6100 = 0x4000(bit14 loopback) | 0x2000(bit13=1 -> 100M) | 0x0100(full duplex)
 * 100M MII clocks come from the PHY's own reference oscillator and run
 * without a cable; 1000BASE-T RX_CLK is recovered from the wire. */
#define YT_LB_BMCR_100M                 0x6100

/* 1000M full duplex + IEEE digital loopback (BMCR bit14).
 *   0x4140 = 0x4000(bit14 loopback) | 0x0040(bit6=1 -> 1000M) | 0x0100(full duplex)
 * The 1000M loopback clock comes from the PHY's PLL, not the wire. */
#define YT_LB_BMCR_1000M_LOOP           0x4140

/* BMSR bits */
#define MII_BMSR_LINK_STATUS            0x0004
#define MII_BMSR_AN_COMPLETE            0x0020

/* SPEC_STATUS bits */
#define FXGMAC_EPHY_LINK_STATUS_BIT     10
#define FXGMAC_EPHY_SPEED_MODE_BIT      11

/* Extended register loopback switches (order sensitive:
 * enable -> sleep -> clean). */
/* Extended register indices (written to REG_MII_EXT_ADDR) and their
 * values — the two are not interchangeable. */
#define REG_MII_EXT_LPBK_REG                    0x0A
#define REG_MII_EXT_SLEEP_CONTROL_REG           0x27
#define REG_MII_EXT_LPBK_REG_ENABLE_LOOPBACK   0x3A18
#define REG_MII_EXT_LPBK_REG_CLEAN_LOOPBACK    0x3A08
#define REG_MII_EXT_SLEEP_REG_ENABLE_LOOPBACK  0x6812
#define REG_MII_EXT_SLEEP_REG_CLEAN_LOOPBACK   0xE812

/* ---- PHY analog front-end (AFE) config ----
 * Must be re-applied after every PHY soft reset (extended registers
 * revert to defaults). Includes keeping CLKDAC_AON (ADC clock) on in
 * AFE_CONTROL_REGISTER3. */
#define REG_MII_SPEC_CTRL                       0x10    /* PHY-specific function control */
#define PHY_MII_SPEC_CTRL_CRS_ON_POS            3
#define PHY_MII_SPEC_CTRL_CRS_ON_LEN            1

#define REG_MII_EXT_AFE_CONTROL_REGISTER3       0x12
#define REG_MII_EXT_AFE_CTRL_CLKDAC_AON_POS     13
#define REG_MII_EXT_AFE_CTRL_CLKDAC_AON_LEN     1
#define REG_MII_EXT_AFE_CTRL_CLKDAC_AON_ON      1

#define REG_MII_EXT_ANALOG_CFG2                 0x51
#define REG_MII_EXT_ANALOG_CFG2_VALUE           0x4a9

#define REG_MII_EXT_ANALOG_CFG3                 0x52
#define MII_EXT_ANALOG_CFG3_ADC_START_CFG_POS   14
#define MII_EXT_ANALOG_CFG3_ADC_START_CFG_LEN   2
#define MII_EXT_ANALOG_CFG3_ADC_START_CFG_DEFAULT 0x0   /* reset default 2, set 0 (VGA bandwidth) */

#define REG_MII_EXT_ANALOG_CFG8                 0x57
#define REG_MII_EXT_ANALOG_CFG8_VALUE           0x274c  /* default board */
#define REG_MII_EXT_ANALOG_CFG8_137D1D05_VALUE  0x264c  /* only for subsystem vendor 0x137d1d05 boards */

/* ---- Chip-level reset and power management (BAR0 direct, no +0x2000) ----
 * Chip reset reloads the EFUSE patch (per-board PHY analog calibration)
 * and raises the PHY reset line; release the PHY again afterwards. */
#define SYS_RESET_REG                           0x152c  /* bit31 chip reset, self-clearing */
#define SYS_RESET_POS                           31
#define REG_PCIE_TRIGGER                        0x1000  /* non-stick region start */
#define MSI_PBA_REG                             0x1300  /* non-stick region end (exclusive) */

#define OOB_WOL_CTRL                            0x1010
#define OOB_WOL_CTRL_DIS_POS                    0       /* 1 = disable OOB WOL */
#define MGMT_WPI_CTRL0                          0x1160
#define MGMT_WPI_CTRL0_WPI_MODE_POS             0
#define MGMT_WPI_CTRL0_WPI_MODE_LEN             2

/* Inside the MAC block (+0x2000) */
#define MAC_PMT_STA_PWRDWN_POS                  0       /* 1 = power-down state, normal RX disabled */

/* Auto-negotiation advertisement bits */
#define FXGMAC_ADVERTISE_10HALF         0x0020
#define FXGMAC_ADVERTISE_10FULL         0x0040
#define FXGMAC_ADVERTISE_100HALF        0x0080
#define FXGMAC_ADVERTISE_100FULL        0x0100
#define FXGMAC_ADVERTISE_1000FULL       0x0200  /* in CTRL1000 */

/* PHY control bit positions */
#define PHY_CR_RESET_POS                15
#define PHY_CR_AUTOENG_POS              12
#define PHY_CR_SPEED_SEL_H_POS          6
#define PHY_CR_SPEED_SEL_L_POS          13
#define PHY_CR_DUPLEX_POS               8

/* ==================== Driver parameters ==================== */
#define YT6801_MAX_PACKET_SIZE          9000
#define YT6801_DEFAULT_MTU              1500
#define YT6801_TX_DESC_CNT              1024
#define YT6801_RX_DESC_CNT              1024

/* ============================================================
 * Datapath extras
 * ============================================================ */

/* --- Extra RX descriptor desc3 bits --- */
#define RDESC3_INTE_POS                 30      /* interrupt on completion */
#define RDESC3_ES_POS                   15      /* error summary, 1 = frame has errors */

/* --- TX descriptor desc2 bitfields --- */
#define TDESC2_HL_B1L_POS               0       /* Buffer1 length (not desc3!) */
#define TDESC2_HL_B1L_LEN               14
#define TDESC2_IC_POS                   31      /* interrupt on completion */

/* --- MAC_CR speed/duplex encoding ---
 * 1000M: PS=0 FES=0 DM=1 / 100M: PS=1 FES=1 DM=1 / 10M: PS=1 FES=0 DM=1
 * (PS=0 FES=1 is the 2500M encoding.) */
#define MAC_CR_DM_POS                   13      /* full duplex */
#define MAC_CR_FES_POS                  14      /* fast ethernet speed */
#define MAC_CR_PS_POS                   15      /* port select: 1=MII, 0=GMII */
#define MAC_CR_ACS_POS                  20      /* automatic pad/CRC strip */
#define MAC_CR_CST_POS                  21      /* CRC stripping */
#define MAC_CR_ARPEN_POS                31      /* ARP offload */

/* --- MAC_ECR bitfields --- */
#define MAC_ECR_DCRCC_POS               16      /* disable RX CRC checking */

/* --- MTL queue registers --- */
#define MTL_Q_BASE                      0x0D00
#define MTL_Q_INC                       0x0040

#define MTL_Q_TQOMR                     0x00    /* TX queue operation mode */
#define MTL_Q_RQOMR                     0x30    /* RX queue operation mode */

#define MTL_Q_TQOMR_TQS_POS             16      /* TX queue FIFO size, in 256-byte units */
#define MTL_Q_TQOMR_TQS_LEN             7
#define MTL_Q_TQOMR_TXQEN_POS           2       /* TX queue enable */
#define MTL_Q_TQOMR_TXQEN_LEN           2
#define MTL_Q_TQOMR_TSF_POS             1       /* TX store-and-forward */
#define MTL_Q_TQOMR_FTQ_POS             0       /* flush transmit queue */

#define MTL_Q_RQOMR_RQS_POS             20      /* RX queue FIFO size, in 256-byte units */
#define MTL_Q_RQOMR_RQS_LEN             9
#define MTL_Q_RQOMR_RFD_POS             14      /* flow control deassert threshold */
#define MTL_Q_RQOMR_RFD_LEN             6
#define MTL_Q_RQOMR_RFA_POS             8       /* flow control assert threshold */
#define MTL_Q_RQOMR_RFA_LEN             6
#define MTL_Q_RQOMR_EHFC_POS            7       /* hardware flow control enable */
#define MTL_Q_RQOMR_RSF_POS             5       /* RX store-and-forward */
#define MTL_Q_RQOMR_FEP_POS             4       /* forward error packets */
#define MTL_Q_RQOMR_FUP_POS             3       /* forward undersized packets */
#define MTL_Q_RQOMR_RTC_POS             0       /* RX threshold control */
#define MTL_Q_RQOMR_RTC_LEN             2

/* TXQEN encoding: 0b00 off, 0b10 enabled; 0b01/0b11 reserved. Writing 3
 * silently leaves the queue disabled — DMA never fetches descriptors. */
#define MTL_QUEUE_DISABLED              0x00
#define MTL_QUEUE_ENABLED               0x02

/* Per-queue FIFO size: computed at runtime from MAC_HWF1R:
 *   q_fifo = min(81920, 1 << (hw_feat + 7)) / queues / 256, minus one. */
#define FXGMAC_MAX_FIFO                 81920
#define MAC_HWF1R_RXFIFOSIZE_POS        0
#define MAC_HWF1R_RXFIFOSIZE_LEN        5
#define MAC_HWF1R_TXFIFOSIZE_POS        6
#define MAC_HWF1R_TXFIFOSIZE_LEN        5

/* The YT6801's internal PHY is a YT8614, PHY ID 0x4f51e91a. */
#define YT6801_PHY_ID1                  0x4F51
#define YT6801_PHY_ID2                  0xE91A

/* ============================================================
 * Datapath sizing
 * ============================================================ */

/* Ring sizes must be powers of two: TDTR/RDTR are device-side address
 * pointers and wraparound relies on masking. */
#define YT6801_TX_RING_DESC             128
#define YT6801_RX_RING_DESC             128

/* RX buffer: 1514 max frame + alignment margin. Must be a multiple of 4
 * (the RBSZ low 2 bits are ignored by hardware). */
#define YT6801_RX_BUFFER_SIZE           2048

/* Minimum received frame length.
 * Smallest valid frame for the stack: 14-byte header + 20-byte minimal
 * IPv4 header = 34. Anything shorter can only be a spurious completion
 * (OWN cleared and PL filled but no data moved). EtherType-0 frames are
 * dropped as well. */
#define YT6801_RX_MIN_FRAME             34

/* Packets in the pool: the RX ring pins 128 long-term, the rest serve
 * TX and stack turnover. */
#define YT6801_POOL_PACKETS             512
#define YT6801_RX_BUF_SIZE              2048    /* DMA_CH_RCR.RBSZ */

/* Loopback self-test */
#define YT6801_LOOPBACK_MAGIC           0x59543638u  /* "YT68" */
#define YT6801_LOOPBACK_TIMEOUT_MS      3000
#define YT6801_PHY_POLL_INTERVAL_MS     100

#endif /* YT6801_REG_H */
