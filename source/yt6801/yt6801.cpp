/*
 * yt6801.cpp — Motorcomm YT6801 Gigabit Ethernet DriverKit driver.
 * PHY internal loopback self-test validates the PCIe->MAC->MDIO->PHY path
 * without a cable.
 */
#include <os/log.h>
#include <time.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>

#include <PCIDriverKit/PCIDriverKit.h>

#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>

/* iig-generated class header; include path must match the one used for the generated .iig.cpp. */
#include <io.miniyv.yt6801ethernet/io_miniyv_yt6801ethernet.h>
#include "yt6801_ivars.h"
#include "yt6801_reg.h"

/* DriverKit's os/log.h has no os_log_create, only OS_LOG_DEFAULT. */
#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "yt6801: " fmt, ##__VA_ARGS__)

typedef io_miniyv_yt6801ethernet_IVars YT;

/* Defined in yt6801_datapath.cpp: program MAC speed from the current link. */
extern uint32_t yt_sync_mac_speed_shared(YT * iv);

/* ==================================================================
 * Register access
 *
 * BAR0 is accessed via PCIDriverKit MemoryRead32/MemoryWrite32; the
 * mapping is already done by IOPCIDevice after GetBARInfo.
 * ================================================================== */
/* MemoryRead32/MemoryWrite32 return void; errors surface as all-1s reads. */
static inline kern_return_t
yt_read32(YT * iv, uint32_t offset, uint32_t * value)
{
    /* All register addresses need the MAC block offset, see yt6801_reg.h */
    iv->pciDevice->MemoryRead32(iv->barIndex, offset + YT6801_MAC_REGS_OFFSET, value);
    return kIOReturnSuccess;
}

static inline kern_return_t
yt_write32(YT * iv, uint32_t offset, uint32_t value)
{
    iv->pciDevice->MemoryWrite32(iv->barIndex, offset + YT6801_MAC_REGS_OFFSET, value);
    return kIOReturnSuccess;
}

/* MGMT block (PHY reset/release, WOL) is not in the MAC block; its
 * addresses are directly relative to BAR0, without the +0x2000. */
static inline kern_return_t
yt_base_read32(YT * iv, uint32_t offset, uint32_t * value)
{
    iv->pciDevice->MemoryRead32(iv->barIndex, offset, value);
    return kIOReturnSuccess;
}

static inline kern_return_t
yt_base_write32(YT * iv, uint32_t offset, uint32_t value)
{
    iv->pciDevice->MemoryWrite32(iv->barIndex, offset, value);
    return kIOReturnSuccess;
}

/* Read-modify-write helper. */
static kern_return_t
yt_set_bits(YT * iv, uint32_t offset, uint32_t mask, bool on)
{
    uint32_t v = 0;
    kern_return_t ret = yt_read32(iv, offset, &v);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    v = on ? (v | mask) : (v & ~mask);
    return yt_write32(iv, offset, v);
}

/* <1ms busy-wait, otherwise sleep. */
static void
yt_delay_us(uint32_t us)
{
    if (us >= 1000) {
        IOSleep(us / 1000);
    } else {
        IODelay(us);
    }
}

/* MDIO indirect access: command word to the address register, poll busy, read/write data. */
static int
yt_mdio_read(YT * iv, uint32_t reg, uint32_t * data)
{
    uint32_t ctrl = (reg << 16) | MAC_MDIO_READ_CMD;
    uint32_t value = 0;
    int busy = MAC_MDIO_POLL_ITERS;

    if (yt_write32(iv, MAC_MDIO_ADDRESS, ctrl) != kIOReturnSuccess) {
        return -1;
    }
    do {
        if (yt_read32(iv, MAC_MDIO_ADDRESS, &value) != kIOReturnSuccess) {
            return -1;
        }
        if ((value & MAC_MDIO_ADDRESS_BUSY) == 0) {
            break;
        }
        /* PHY MDIO transactions take tens of us; add an explicit delay. */
        IODelay(10);
        busy--;
    } while (busy > 0);

    if (value & MAC_MDIO_ADDRESS_BUSY) {
        LOG("MDIO read timeout reg=0x%02x ctrl=0x%08x", reg, value);
        return -1;
    }
    if (yt_read32(iv, MAC_MDIO_DATA, &value) != kIOReturnSuccess) {
        return -1;
    }
    *data = value & 0xFFFF;
    iv->mdioReads++;
    return 0;
}

static int
yt_mdio_write(YT * iv, uint32_t reg, uint32_t data)
{
    uint32_t ctrl = (reg << 16) | MAC_MDIO_WRITE_CMD;
    uint32_t value = 0;
    int busy = MAC_MDIO_POLL_ITERS;

    if (yt_write32(iv, MAC_MDIO_DATA, data) != kIOReturnSuccess) {
        return -1;
    }
    if (yt_write32(iv, MAC_MDIO_ADDRESS, ctrl) != kIOReturnSuccess) {
        return -1;
    }
    do {
        if (yt_read32(iv, MAC_MDIO_ADDRESS, &value) != kIOReturnSuccess) {
            return -1;
        }
        if ((value & MAC_MDIO_ADDRESS_BUSY) == 0) {
            break;
        }
        IODelay(10);
        busy--;
    } while (busy > 0);

    if (value & MAC_MDIO_ADDRESS_BUSY) {
        LOG("MDIO write timeout reg=0x%02x", reg);
        return -1;
    }
    iv->mdioWrites++;
    return 0;
}

/* Extended registers: write the address to 0x1E, then access 0x1F. */
static int
yt_ext_write(YT * iv, uint32_t extReg, uint32_t value)
{
    if (yt_mdio_write(iv, REG_MII_EXT_ADDR, extReg) != 0) {
        return -1;
    }
    return yt_mdio_write(iv, REG_MII_EXT_DATA, value);
}

static int
yt_ext_read(YT * iv, uint32_t extReg, uint32_t * value)
{
    if (yt_mdio_write(iv, REG_MII_EXT_ADDR, extReg) != 0) {
        return -1;
    }
    return yt_mdio_read(iv, REG_MII_EXT_DATA, value);
}

/* ==================================================================
 * EFUSE factory MAC read.
 * Patch table: 6-byte entries (2-byte id + 4-byte value) starting at
 * EFUSE row 18. Per byte: write row+START+ROW_READ to CTRL_0, poll
 * CTRL_1 DONE (max 1000 tries, 20-50us each); data is CTRL_1 bits
 * 31:24. Id 0x0000 ends the table; 0x1520/0x1524 map to MACA0LR/
 * MACA0HR (table ids, not MMIO addresses).
 * ================================================================== */
static bool
yt_efuse_read_byte(YT * iv, uint32_t row, uint8_t * out)
{
    uint32_t v = ((row & 0xFFu) << EFUSE_OP_ADDR_POS)
               | (1u << EFUSE_OP_START_POS)
               | (EFUSE_OP_MODE_ROW_READ << EFUSE_OP_MODE_POS);

    yt_base_write32(iv, EFUSE_OP_CTRL_0, v);

    for (int wait = 1000; wait > 0; wait--) {
        IODelay(20);
        if (yt_base_read32(iv, EFUSE_OP_CTRL_1, &v) != kIOReturnSuccess) {
            return false;
        }
        if ((v >> EFUSE_OP_DONE_POS) & 1u) {
            *out = (uint8_t)((v >> EFUSE_OP_RD_DATA_POS) & 0xFFu);
            return true;
        }
    }
    return false;
}

/* Read patch entry index: 2-byte id + 4-byte value. */
static bool
yt_efuse_read_patch(YT * iv, uint8_t index, uint16_t * id, uint32_t * value)
{
    uint32_t base = EFUSE_REGION_A_B_LENGTH
                  + (uint32_t)index * EFUSE_EACH_PATH_SIZE;
    uint32_t idv = 0, val = 0;
    uint8_t  b = 0;

    for (uint32_t i = EFUSE_PATCH_ADDR_START_BYTE;
         i < EFUSE_PATCH_DATA_START_BYTE; i++) {
        if (!yt_efuse_read_byte(iv, base + i, &b)) {
            return false;
        }
        idv |= (uint32_t)b << (i * 8);
    }
    for (uint32_t i = EFUSE_PATCH_DATA_START_BYTE;
         i < EFUSE_EACH_PATH_SIZE; i++) {
        if (!yt_efuse_read_byte(iv, base + i, &b)) {
            return false;
        }
        val |= (uint32_t)b << ((i - EFUSE_PATCH_DATA_START_BYTE) * 8);
    }

    *id = (uint16_t)idv;
    *value = val;
    return true;
}

/*
 * Fetch the MAC from the EFUSE patch table. Returns false on any
 * failure (EFUSE unreadable, no MAC entry, all 0/all FF, multicast bit
 * set); the caller then falls back to the legacy path.
 */
static bool
yt_mac_read_from_efuse(YT * iv, uint8_t mac[YT6801_MAC_LEN])
{
    uint32_t maclr = 0, machr = 0;
    bool got_lr = false, got_hr = false;

    for (uint8_t index = 0; index < EFUSE_MAX_ENTRY; index++) {
        uint16_t id = 0;
        uint32_t value = 0;

        if (!yt_efuse_read_patch(iv, index, &id, &value)) {
            LOG("EFUSE entry %{public}u read failed, giving up on EFUSE", index);
            return false;
        }
        if (id == 0x0000) {
            break;                      /* blank entry, end of table */
        }
        if (id == EFUSE_MAC_LR_ID) {
            maclr = value;
            got_lr = true;
        }
        if (id == EFUSE_MAC_HR_ID) {
            machr = value;
            got_hr = true;
        }
    }

    if (!got_lr || !got_hr) {
        LOG("EFUSE patch table has no MAC entry (lr=%u hr=%u)",
            got_lr ? 1u : 0u, got_hr ? 1u : 0u);
        return false;
    }

    /* Assembly order is fixed: LR holds mac[5..2], HR bits 15:8 hold mac[0]. */
    mac[5] = (uint8_t)(maclr & 0xFF);
    mac[4] = (uint8_t)((maclr >> 8) & 0xFF);
    mac[3] = (uint8_t)((maclr >> 16) & 0xFF);
    mac[2] = (uint8_t)((maclr >> 24) & 0xFF);
    mac[1] = (uint8_t)(machr & 0xFF);
    mac[0] = (uint8_t)((machr >> 8) & 0xFF);

    uint32_t sum = 0;
    for (int i = 0; i < YT6801_MAC_LEN; i++) {
        sum += mac[i];
    }
    if (sum == 0 || sum == 0xFF * YT6801_MAC_LEN) {
        LOG("EFUSE MAC all 0/all FF, invalid");
        return false;
    }
    if (mac[0] & 0x01) {
        LOG("EFUSE MAC is multicast, invalid");
        return false;
    }
    return true;
}

/* ==================================================================
 * MAC address bit layout:
 * MAC_MACA0HR: bit15:8 = mac[5], bit7:0 = mac[4]
 * MAC_MACA0LR: bit7:0 = mac[0], bit15:8 = mac[1],
 *              bit23:16 = mac[2], bit31:24 = mac[3]
 * ================================================================== */
static void
yt_mac_read_address(YT * iv)
{
    uint8_t  efuseMac[YT6801_MAC_LEN] = { 0 };
    bool     valid = false;

    /* ---- Source 1 (highest priority): factory address in EFUSE.
     * Fall back to the legacy path if unreadable or invalid. ---- */
    if (yt_mac_read_from_efuse(iv, efuseMac)) {
        for (int i = 0; i < YT6801_MAC_LEN; i++) {
            iv->macAddress[i] = efuseMac[i];
        }
        valid = true;
        LOG("MAC source: EFUSE factory address = %{public}02x:%{public}02x:%{public}02x:%{public}02x:%{public}02x:%{public}02x",
            iv->macAddress[0], iv->macAddress[1], iv->macAddress[2],
            iv->macAddress[3], iv->macAddress[4], iv->macAddress[5]);
    }

    /* ---- Source 2: MAC address registers MACA0 (may be programmed by BIOS). ---- */
    if (!valid) {
        uint32_t hi = 0, lo = 0;

        if (yt_read32(iv, MAC_MACA0HR, &hi) == kIOReturnSuccess &&
            yt_read32(iv, MAC_MACA0LR, &lo) == kIOReturnSuccess) {
            iv->macAddress[0] = (uint8_t)(lo & 0xFF);
            iv->macAddress[1] = (uint8_t)((lo >> 8) & 0xFF);
            iv->macAddress[2] = (uint8_t)((lo >> 16) & 0xFF);
            iv->macAddress[3] = (uint8_t)((lo >> 24) & 0xFF);
            iv->macAddress[4] = (uint8_t)(hi & 0xFF);
            iv->macAddress[5] = (uint8_t)((hi >> 8) & 0xFF);

            uint32_t sum = 0;
            for (int i = 0; i < YT6801_MAC_LEN; i++) {
                sum += iv->macAddress[i];
            }
            valid = (sum != 0) && (sum != 0xFF * YT6801_MAC_LEN);
            if (valid) {
                LOG("MAC source: MACA0 registers = %{public}02x:%{public}02x:%{public}02x:%{public}02x:%{public}02x:%{public}02x",
                    iv->macAddress[0], iv->macAddress[1], iv->macAddress[2],
                    iv->macAddress[3], iv->macAddress[4], iv->macAddress[5]);
            }
        }
    }

    /* ---- Source 3: fallback locally administered address. ---- */
    if (!valid) {
        LOG("No valid address in EFUSE or MACA0, falling back to locally administered");
        static const uint8_t fallback[YT6801_MAC_LEN] = { 0x02, 0x59, 0x54, 0x36, 0x80, 0x01 };
        for (int i = 0; i < YT6801_MAC_LEN; i++) {
            iv->macAddress[i] = fallback[i];
        }
        valid = true;
    }

    iv->macValid = valid;

    /* Write the final address into MAC address filter 0 (MACA0LR/HR). */
    {
        uint32_t lo = (uint32_t)iv->macAddress[0]
                    | ((uint32_t)iv->macAddress[1] << 8)
                    | ((uint32_t)iv->macAddress[2] << 16)
                    | ((uint32_t)iv->macAddress[3] << 24);
        uint32_t hi = (uint32_t)iv->macAddress[4]
                    | ((uint32_t)iv->macAddress[5] << 8);
        yt_write32(iv, MAC_MACA0LR, lo);
        yt_write32(iv, MAC_MACA0HR, hi);
        LOG("MAC filter written %02x:%02x:%02x:%02x:%02x:%02x",
            iv->macAddress[0], iv->macAddress[1], iv->macAddress[2],
            iv->macAddress[3], iv->macAddress[4], iv->macAddress[5]);
    }
}

/* ==================================================================
 * DMA soft reset — set DMA_MR.SWR and wait for self-clear
 * ================================================================== */
static kern_return_t
yt_dma_soft_reset(YT * iv)
{
    kern_return_t ret;
    uint32_t v = 0;

    ret = yt_set_bits(iv, DMA_MR, (1u << DMA_MR_SWR_POS), true);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    for (int i = 0; i < 100; i++) {
        yt_delay_us(200);
        if (yt_read32(iv, DMA_MR, &v) != kIOReturnSuccess) {
            return kIOReturnError;
        }
        if ((v & (1u << DMA_MR_SWR_POS)) == 0) {
            return kIOReturnSuccess;
        }
    }
    LOG("DMA soft reset timeout DMA_MR=0x%08x", v);
    return kIOReturnTimeout;
}

static kern_return_t
yt_phy_analog_init(YT * iv);

static void
yt_powerup_config(YT * iv);

static kern_return_t
yt_hardware_init(YT * iv)
{
    kern_return_t ret;
    uint32_t v = 0;

    /* ---- 1. Chip-level reset: SYS_RESET_REG(0x152c) bit31, self-clearing.
     * Reloads the EFUSE patch table and raises the PHY reset line.
     * Non-stick BAR0 registers 0x1000~0x1300 are cleared: save/restore. */
    {
        uint32_t nonstick[(MSI_PBA_REG - REG_PCIE_TRIGGER) / 4];
        for (uint32_t off = REG_PCIE_TRIGGER, i = 0; off < MSI_PBA_REG; off += 4, i++) {
            yt_base_read32(iv, off, &nonstick[i]);
        }

        uint32_t rst = 0;
        yt_base_read32(iv, SYS_RESET_REG, &rst);
        LOG("SYS_RESET=0x%08x before chip reset", rst);
        yt_base_write32(iv, SYS_RESET_REG, rst | (1u << SYS_RESET_POS));
        yt_delay_us(10000);                     /* 10ms */

        for (uint32_t off = REG_PCIE_TRIGGER, i = 0; off < MSI_PBA_REG; off += 4, i++) {
            yt_base_write32(iv, off, nonstick[i]);
        }

        /* The chip reset raised the PHY reset line (0x1004=0x16); release. */
        uint32_t ctrl = 0;
        yt_base_read32(iv, MGMT_EPHY_CTRL, &ctrl);
        LOG("MGMT_EPHY_CTRL=0x%08x after chip reset (reset state 0x16)", ctrl);
        yt_base_write32(iv, MGMT_EPHY_CTRL, 0x1u);   /* release */
        yt_delay_us(150);
    }

    yt_read32(iv, MAC_VR, &v);
    LOG("MAC version register = 0x%08x", v);

    ret = yt_dma_soft_reset(iv);
    if (ret != kIOReturnSuccess) {
        LOG("DMA soft reset failed 0x%x", ret);
        return ret;
    }
    LOG("DMA soft reset done");

    /* PHY reset/release at BAR0+0x1004 (MGMT block, no +0x2000).
     * bit0: 0 = hold, 1 = release. Sequence: 0, wait 1.5ms, 1. */
    {
        uint32_t ctrl = 0;
        yt_base_read32(iv, MGMT_EPHY_CTRL, &ctrl);
        LOG("MGMT_EPHY_CTRL=0x%08x rst=%u", ctrl, ctrl & 1u);

        yt_base_write32(iv, MGMT_EPHY_CTRL, 0x0u);      /* hold reset */
        yt_delay_us(1500);
        yt_base_write32(iv, MGMT_EPHY_CTRL, 0x1u);      /* release */
        yt_delay_us(300);

        uint32_t after = 0;
        yt_base_read32(iv, MGMT_EPHY_CTRL, &after);
        LOG("PHY released MGMT_EPHY_CTRL=0x%08x rst=%u link=%u speed=%u",
            after, after & 1u, (after >> 1) & 1u, (after >> 3) & 3u);
    }

    /* PHY analog front-end config; re-apply after every PHY soft reset. */
    (void)yt_phy_analog_init(iv);

    /* ---- 3. Power management sequence ---- */
    yt_powerup_config(iv);

    /* Disable RX/TX before handing a clean state to the upper layer. */
    yt_set_bits(iv, MAC_CR, (1u << MAC_CR_RE_POS), false);
    yt_set_bits(iv, MAC_CR, (1u << MAC_CR_TE_POS), false);

    /* Write 1 to clear all interrupt status bits. */
    yt_write32(iv, MAC_ISR, 0xFFFFFFFFu);
    yt_write32(iv, DMA_ISR, 0xFFFFFFFFu);

    iv->hardwareReady = true;
    return kIOReturnSuccess;
}

/* PHY analog front-end config; must be re-applied after every PHY soft reset. */
static kern_return_t
yt_phy_analog_init(YT * iv)
{
    uint32_t val = 0;

    if (iv == nullptr || !iv->pciOpened) {
        return kIOReturnNotReady;
    }

    /* 1. SPEC_CTRL: enable CRS. */
    if (yt_mdio_read(iv, REG_MII_SPEC_CTRL, &val) == 0) {
        val |= (1u << PHY_MII_SPEC_CTRL_CRS_ON_POS);
        (void)yt_mdio_write(iv, REG_MII_SPEC_CTRL, val);
    }
    /* 2. ANALOG_CFG3: VGA bandwidth, reset default 2 -> 0. */
    if (yt_ext_read(iv, REG_MII_EXT_ANALOG_CFG3, &val) == 0) {
        val &= ~(((1u << MII_EXT_ANALOG_CFG3_ADC_START_CFG_LEN) - 1u)
                 << MII_EXT_ANALOG_CFG3_ADC_START_CFG_POS);
        (void)yt_ext_write(iv, REG_MII_EXT_ANALOG_CFG3, val);
    }
    /* 3. ANALOG_CFG2: fixed whole-word value. */
    (void)yt_ext_write(iv, REG_MII_EXT_ANALOG_CFG2, REG_MII_EXT_ANALOG_CFG2_VALUE);
    /* 4. ANALOG_CFG8: fixed whole-word value. */
    (void)yt_ext_write(iv, REG_MII_EXT_ANALOG_CFG8, REG_MII_EXT_ANALOG_CFG8_VALUE);
    /* 5. AFE_CONTROL_REGISTER3: CLKDAC_AON always on. */
    if (yt_ext_read(iv, REG_MII_EXT_AFE_CONTROL_REGISTER3, &val) == 0) {
        val |= (REG_MII_EXT_AFE_CTRL_CLKDAC_AON_ON
                << REG_MII_EXT_AFE_CTRL_CLKDAC_AON_POS);
        (void)yt_ext_write(iv, REG_MII_EXT_AFE_CONTROL_REGISTER3, val);
    }

    uint32_t afe3 = 0, cfg2 = 0, cfg8 = 0;
    (void)yt_ext_read(iv, REG_MII_EXT_AFE_CONTROL_REGISTER3, &afe3);
    (void)yt_ext_read(iv, REG_MII_EXT_ANALOG_CFG2, &cfg2);
    (void)yt_ext_read(iv, REG_MII_EXT_ANALOG_CFG8, &cfg8);
    LOG("PHY analog front-end config done AFE3=0x%04x CFG2=0x%04x CFG8=0x%04x",
        afe3, cfg2, cfg8);
    return kIOReturnSuccess;
}

/* Power management sequence; clears the PMT power-down bit. */
static void
yt_powerup_config(YT * iv)
{
    uint32_t val = 0;

    /* 1. Disable OOB WOL (EFUSE may have it on). */
    yt_base_read32(iv, OOB_WOL_CTRL, &val);
    LOG("OOB_WOL=0x%08x (bit0 was %u)", val, val & 1u);
    val |= (1u << OOB_WOL_CTRL_DIS_POS);
    yt_base_write32(iv, OOB_WOL_CTRL, val);

    /* 2. Clear WPI mode (write the reset value). */
    yt_base_read32(iv, MGMT_WPI_CTRL0, &val);
    val &= ~((1u << MGMT_WPI_CTRL0_WPI_MODE_LEN) - 1u);
    yt_base_write32(iv, MGMT_WPI_CTRL0, val);

    /* 3. Clear PMT power-down + read once to clear the interrupt. */
    yt_read32(iv, MAC_PMT_STA, &val);
    LOG("MAC_PMT_STA=0x%08x (bit0 power-down was %u)", val, val & 1u);
    val &= ~(1u << MAC_PMT_STA_PWRDWN_POS);
    yt_write32(iv, MAC_PMT_STA, val);
    (void)val;
}

/* ==================================================================
 * PHY internal loopback self-test (no cable needed).
 * Read PHY ID, read BMSR, set BMCR 0x8140, set ext regs 0x3A18/0x6812,
 * check SPEC_STATUS, restore with 0x9140.
 * Result bitmap: bit0 PHY ID readable, bit1 loopback set, bit2 ext regs
 * writable, bit3 SPEC_STATUS readable, bit4 restore ok. 0 = all passed.
 * ================================================================== */
static kern_return_t
yt_phy_loopback_test(YT * iv, uint64_t * result)
{
    uint32_t physid1 = 0, physid2 = 0;
    uint32_t bmsr = 0, bmcr = 0, status = 0, ext = 0;
    uint64_t bits = 0;

    if (yt_mdio_read(iv, REG_MII_PHYSID1, &physid1) != 0 ||
        yt_mdio_read(iv, REG_MII_PHYSID2, &physid2) != 0) {
        LOG("loopback test failed: PHY registers unreadable (MDIO no response)");
        if (result) {
            *result = ~0ull;
        }
        return kIOReturnNoDevice;
    }
    iv->phyId1 = (uint16_t)physid1;
    iv->phyId2 = (uint16_t)physid2;
    LOG("PHY ID = 0x%04x:0x%04x", iv->phyId1, iv->phyId2);

    if (physid1 == 0 && physid2 == 0) {
        LOG("loopback test failed: PHY ID all zero, hardware not ready");
        if (result) {
            *result = ~0ull;
        }
        return kIOReturnNoDevice;
    }
    if (physid1 != YT6801_PHY_ID1 || physid2 != YT6801_PHY_ID2) {
        LOG("warning: PHY ID does not match YT8614 (0x%04x:0x%04x), continuing",
            YT6801_PHY_ID1, YT6801_PHY_ID2);
    } else {
        LOG("PHY is YT8614 (YT6801 internal), MDIO path OK");
    }

    if (yt_mdio_read(iv, REG_MII_BMSR, &bmsr) == 0) {
        LOG("BMSR=0x%04x link=%u an_done=%u",
            bmsr,
            (bmsr & MII_BMSR_LINK_STATUS) ? 1u : 0u,
            (bmsr & MII_BMSR_AN_COMPLETE) ? 1u : 0u);
    }

    /* --- Enable loopback --- */
    if (yt_mdio_read(iv, REG_MII_BMCR, &bmcr) != 0) {
        LOG("loopback test failed: BMCR read failed");
        if (result) {
            *result = ~0ull;
        }
        return kIOReturnIOError;
    }
    LOG("BMCR before loopback=0x%04x", bmcr);

    if (yt_mdio_write(iv, REG_MII_BMCR, REG_MII_BMCR_ENABLE_LOOPBACK) != 0) {
        LOG("loopback test failed: BMCR=0x%04x write failed", REG_MII_BMCR_ENABLE_LOOPBACK);
        if (result) {
            *result = ~0ull;
        }
        return kIOReturnIOError;
    }
    iv->loopbackActive = true;
    yt_delay_us(1000);

    if (yt_mdio_read(iv, REG_MII_BMCR, &bmcr) != 0) {
        LOG("loopback test failed: BMCR re-read failed");
        bits |= (1ull << 1);
    } else {
        /* bit15 self-clears on write; loopback validity comes from ext reg 0x0A. */
        LOG("loopback on, BMCR readback=0x%04x (bit15 self-clear is normal)", bmcr);
    }

    /* --- Extended registers: the real loopback switches ---
     * EXT 0x0A=0x3A18 (LPBK_REG), EXT 0x27=0x6812 (SLEEP_CONTROL),
     * BMCR=0x8140. */
    if (yt_ext_write(iv, REG_MII_EXT_LPBK_REG, REG_MII_EXT_LPBK_REG_ENABLE_LOOPBACK) != 0) {
        LOG("warning: ext loopback register 0x0A<-0x3A18 write failed");
        bits |= (1ull << 2);
    }
    if (yt_ext_write(iv, REG_MII_EXT_SLEEP_CONTROL_REG, REG_MII_EXT_SLEEP_REG_ENABLE_LOOPBACK) != 0) {
        LOG("warning: ext sleep register 0x27<-0x6812 write failed");
        bits |= (1ull << 2);
    }
    if (yt_ext_read(iv, REG_MII_EXT_LPBK_REG, &ext) == 0) {
        LOG("ext loopback register 0x0A readback = 0x%04x (expect 0x3A18)", ext);
    }

    /* --- Link state --- */
    if (yt_mdio_read(iv, REG_MII_SPEC_STATUS, &status) == 0) {
        iv->linkUp = ((status >> FXGMAC_EPHY_LINK_STATUS_BIT) & 1u) != 0;
        LOG("SPEC_STATUS=0x%04x link=%u speed_bit=%u",
            status,
            iv->linkUp ? 1u : 0u,
            (status >> FXGMAC_EPHY_SPEED_MODE_BIT) & 1u);
    } else {
        bits |= (1ull << 3);
    }

    /* --- Unconditional restore --- */
    if (yt_mdio_write(iv, REG_MII_BMCR, REG_MII_BMCR_DISABLE_LOOPBACK) == 0) {
        LOG("loopback off BMCR=0x%04x", REG_MII_BMCR_DISABLE_LOOPBACK);
    } else {
        LOG("warning: failed to disable loopback");
        bits |= (1ull << 4);
    }
    iv->loopbackActive = false;

    LOG("PHY internal loopback test %s (MDIO reads %u, writes %u)",
        bits == 0 ? "passed" : "partially failed", iv->mdioReads, iv->mdioWrites);

    if (result) {
        *result = bits;
    }
    return bits == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

/* ==================================================================
 * Real-link PHY status decoding, from SPEC_STATUS:
 *   bit10 link, bit13 duplex, bit14:15 speed (0=10M, 1=100M, 2=1000M)
 * ================================================================== */
#define YT_SPEC_LINK_POS        10
#define YT_SPEC_DUPLEX_POS      13
#define YT_SPEC_SPEED_POS       14
#define YT_SPEC_SPEED_MASK      0x3u

struct YTLinkState {
    bool     up;
    bool     fullDuplex;
    uint32_t speedMbps;     /* 10 / 100 / 1000; 0 = unreadable */
};

static void
yt_decode_link(uint32_t spec, YTLinkState * out)
{
    out->up         = ((spec >> YT_SPEC_LINK_POS) & 1u) != 0;
    out->fullDuplex = ((spec >> YT_SPEC_DUPLEX_POS) & 1u) != 0;
    switch ((spec >> YT_SPEC_SPEED_POS) & YT_SPEC_SPEED_MASK) {
        case 0:  out->speedMbps = 10;   break;
        case 1:  out->speedMbps = 100;  break;
        case 2:  out->speedMbps = 1000; break;
        default: out->speedMbps = 0;    break;
    }
}

/* One SPEC_STATUS read + decode. Returns false when the MDIO read
 * failed; the caller must not treat that as "link down". */
static bool
yt_read_link(YT * iv, YTLinkState * out)
{
    uint32_t spec = 0;

    out->up = false;
    out->fullDuplex = false;
    out->speedMbps = 0;
    if (iv == nullptr) {
        return false;
    }
    if (yt_mdio_read(iv, REG_MII_SPEC_STATUS, &spec) != 0) {
        return false;
    }
    yt_decode_link(spec, out);
    return true;
}

/* Wait for auto-negotiation; logs SPEC_STATUS every 3s while waiting. */
static bool
yt_wait_link(YT * iv, uint32_t timeoutMs)
{
    YTLinkState ls;
    uint32_t waited = 0;
    uint32_t spec = 0;

    for (;;) {
        if (yt_mdio_read(iv, REG_MII_SPEC_STATUS, &spec) == 0) {
            yt_decode_link(spec, &ls);
            if (ls.up) {
                return true;
            }
            if (waited > 0 && (waited % 3000) == 0) {
                LOG("link wait %ums: SPEC_STATUS=0x%08x (bit10=link)", waited, spec);
            }
        }
        if (waited >= timeoutMs) {
            LOG("link wait timeout (%ums): last SPEC_STATUS=0x%08x", timeoutMs, spec);
            return false;
        }
        yt_delay_us(100000);        /* poll every 100ms */
        waited += 100;
    }
}

/* ==================================================================
 * Loopback window + link reporting.
 * No cable: PHY internal loopback, link reported ACTIVE so the stack
 * still delivers frames; PHY restored afterwards.
 * ================================================================== */
#ifndef YT6801_LOOPBACK_WINDOW_SEC
#define YT6801_LOOPBACK_WINDOW_SEC 180
#endif

kern_return_t
io_miniyv_yt6801ethernet::enterPhyLoopback()
{
    YT * iv = ivars;
    uint32_t bmcr = 0;
    uint32_t status = 0;

    if (iv == nullptr || !iv->pciOpened) {
        return kIOReturnNotReady;
    }
    if (iv->loopbackActive) {
        return kIOReturnSuccess;
    }

    if (yt_mdio_read(iv, REG_MII_BMCR, &bmcr) != 0) {
        LOG("enter loopback failed: BMCR unreadable");
        return kIOReturnIOError;
    }
    iv->bmcrSaved = (uint16_t)bmcr;

    /* Three steps, order fixed:
     *   1. EXT 0x27 <- 0x6812  sleep control: no sleep + loopback on
     *   2. EXT 0x0A <- 0x3A18  loopback register: digital loopback on
     *   3. BMCR    <- 0x8140   bit15 soft reset loads the two writes */
    yt_ext_write(iv, REG_MII_EXT_SLEEP_CONTROL_REG, REG_MII_EXT_SLEEP_REG_ENABLE_LOOPBACK);
    yt_ext_write(iv, REG_MII_EXT_LPBK_REG, REG_MII_EXT_LPBK_REG_ENABLE_LOOPBACK);
    if (yt_mdio_write(iv, REG_MII_BMCR, YT_LB_BMCR_1000M_LOOP) != 0) {
        LOG("loopback setup failed: BMCR write failed");
        return kIOReturnIOError;
    }
    iv->loopbackActive = true;
    iv->loopbackBmcr = YT_LB_BMCR_1000M_LOOP;

    /* Wait up to 2s for the loopback link bit after the PHY soft reset. */
    {
        bool up = false;
        int  waited = 0;
        uint32_t ext = 0;

        for (int i = 0; i < 100; i++) {
            yt_delay_us(20000);
            waited += 20;
            if (yt_mdio_read(iv, REG_MII_SPEC_STATUS, &status) != 0) {
                continue;
            }
            if (((status >> FXGMAC_EPHY_LINK_STATUS_BIT) & 1u) != 0) {
                up = true;
                break;
            }
        }
        (void)yt_ext_read(iv, REG_MII_EXT_LPBK_REG, &ext);
        {
            uint32_t bmsr = 0;
            (void)yt_mdio_read(iv, REG_MII_BMSR, &bmsr);
            if (!up && (bmsr & MII_BMSR_LINK_STATUS) != 0) {
                up = true;      /* the standard register counts too */
            }
            LOG("PHY loopback ready: EXT 0x0A=0x%04x BMSR=0x%04x SPEC_STATUS=0x%04x link=%u (waited %d ms)",
                ext, bmsr, status, up ? 1u : 0u, waited);
        }

        /* MAC-side PHY interface status: bit1=link, bit2=duplex, bit3..4=speed. */
        {
            uint32_t mgmt = 0;
            (void)yt_base_read32(iv, MGMT_EPHY_CTRL, &mgmt);
            LOG("MAC-side interface under loopback MGMT_EPHY_CTRL=0x%08x link=%u duplex=%u speed=%u",
                mgmt, (mgmt >> MGMT_EPHY_CTRL_LINKUP_POS) & 1u,
                (mgmt >> 2) & 1u, (mgmt >> MGMT_EPHY_CTRL_SPEED_POS) & 3u);
        }
        if (!up) {
            /* MAC internal loopback does not depend on the PHY link. */
            LOG("note: link bit not up under PHY loopback, RX clock may be missing; MAC internal loopback still possible");
        }
    }
    (void)bmcr;
    return kIOReturnSuccess;
}

kern_return_t
io_miniyv_yt6801ethernet::exitPhyLoopback()
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->pciOpened) {
        return kIOReturnNotReady;
    }

    /* Restore: ext registers first, then BMCR (auto-neg restart, no soft reset). */
    yt_ext_write(iv, REG_MII_EXT_LPBK_REG, REG_MII_EXT_LPBK_REG_CLEAN_LOOPBACK);
    yt_ext_write(iv, REG_MII_EXT_SLEEP_CONTROL_REG, REG_MII_EXT_SLEEP_REG_CLEAN_LOOPBACK);
    if (yt_mdio_write(iv, REG_MII_BMCR, REG_MII_BMCR_RESTORE_ANEG) != 0) {
        LOG("exit loopback failed: BMCR restore write failed");
        return kIOReturnIOError;
    }
    iv->loopbackActive = false;
    iv->loopbackWindowOpen = false;
    yt_delay_us(20000);
    LOG("PHY loopback restored (BMCR=0x%04x)", REG_MII_BMCR_DISABLE_LOOPBACK);
    return kIOReturnSuccess;
}

void
io_miniyv_yt6801ethernet::reportLink(bool up)
{
    YT * iv = ivars;
    kern_return_t ret;

    if (iv == nullptr || !iv->netRegistered) {
        return;
    }

    /* Report the negotiated speed/duplex; self-loop mode reports 1000M. */
    MediaWord media;
    if (iv->selfLoopMode) {
        media = (MediaWord)(kIOUserNetworkMediaEthernet1000BaseT
                          | kIOUserNetworkMediaOptionFullDuplex);
    } else {
        YTLinkState ls;
        (void)yt_read_link(iv, &ls);
        if (ls.up && ls.speedMbps != 0) {
            iv->linkSpeedMbps  = ls.speedMbps;
            iv->linkFullDuplex = ls.fullDuplex;
        }

        MediaWord base = (MediaWord)kIOUserNetworkMediaEthernetAuto;
        if (ls.speedMbps == 10) {
            base = (MediaWord)kIOUserNetworkMediaEthernet10BaseT;
        } else if (ls.speedMbps == 100) {
            base = (MediaWord)kIOUserNetworkMediaEthernet100BaseTX;
        } else if (ls.speedMbps == 1000) {
            base = (MediaWord)kIOUserNetworkMediaEthernet1000BaseT;
        }
        media = (MediaWord)(base
                          | (ls.fullDuplex ? kIOUserNetworkMediaOptionFullDuplex
                                           : kIOUserNetworkMediaOptionHalfDuplex));
    }
    LinkStatus st = up ? (LinkStatus)kIOUserNetworkLinkStatusActive
                       : (LinkStatus)kIOUserNetworkLinkStatusInactive;

    ret = reportLinkStatus(st, media);
    reportLinkQuality(up ? (LinkQuality)kIOUserNetworkLinkQualityGood
                         : (LinkQuality)kIOUserNetworkLinkQualityOff);

    iv->linkUp = up;
    iv->linkReports++;
    LOG("link %{public}s (media=0x%x %{public}s rc=0x%x)", up ? "UP" : "DOWN", media,
        iv->selfLoopMode ? "self-loop mode" : "real link", ret);
}

/* Poll timer: reclaim TX + service RX, then re-arm the 5ms tick. */
void
io_miniyv_yt6801ethernet::PollTimerOccurred_Impl(OSAction * action, uint64_t time)
{
    YT * iv = ivars;
    if (iv == nullptr || iv->pollTimer == nullptr) {
        return;
    }
    if (iv->datapathReady) {
        reclaimTx();
        serviceRx();
    }
    /* Periodic link check: PHYIS never fires (no working interrupts), so
     * the 5ms tick drives link detection. Cheap SPEC_STATUS read; the
     * loopback probe (exit/wait/re-enter) runs at a slower 2s rate. */
    iv->linkPollTick++;
    if ((iv->linkPollTick % (iv->loopbackActive ? 400u : 40u)) == 0) {
        pollLinkStatus();
    }
    /* Adaptive tick: 1 ms while RX traffic flows, 5 ms when idle.
     * A fixed 5 ms tick adds ~2.5 ms average RX latency, which pins
     * TCP throughput near window/RTT (~15 Mbps on a 1G link). */
    uint64_t period;
    if (iv->rxHarvested != 0) {
        iv->pollIdle = 0;
        period = 1000000ull;
    } else if (++iv->pollIdle < 100u) {
        period = 1000000ull;
    } else {
        period = 5000000ull;
    }
    iv->rxHarvested = 0;
    /* Re-arm regardless of datapath state, keep the heartbeat. */
    (void)iv->pollTimer->WakeAtTime(
        kIOTimerClockUptimeRaw,
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + period, 0);
}

/* Read SPEC_STATUS and report on change; triggered by MAC_ISR.PHYIS.
 * While loopback is active: exit first, re-enter if still no cable. */
void
io_miniyv_yt6801ethernet::pollLinkStatus()
{
    YT * iv = ivars;
    YTLinkState ls;

    if (iv == nullptr || !iv->netRegistered || !iv->pciOpened) {
        return;
    }

    if (iv->loopbackActive) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

        if (iv->loopbackProbeStart == 0) {
            /* Open a probe window: leave loopback and let auto-neg run.
             * The old code checked the link 500ms after a PHY soft reset, so a
             * cable plugged in while in loopback could never be detected. */
            if (exitPhyLoopback() != kIOReturnSuccess) {
                return;
            }
            iv->loopbackProbeStart = now;
            return;
        }
        if (yt_read_link(iv, &ls) && ls.up) {
            iv->loopbackProbeStart = 0;
            /* Cable present: undo self-loop-mode leftovers. */
            yt_set_bits(iv, MAC_CR, (1u << MAC_CR_LM_POS), false);
            yt_set_bits(iv, MAC_PFR,
                        (1u << MAC_PFR_PR_POS) | (1u << MAC_PFR_RA_POS), false);
            iv->selfLoopMode = false;
            {
                uint32_t cr = 0, pfr = 0;
                (void)yt_read32(iv, MAC_CR, &cr);
                (void)yt_read32(iv, MAC_PFR, &pfr);
                LOG("real link detected, exited loopback mode (MAC_CR=0x%08x PFR=0x%08x)", cr, pfr);
            }
            if (ls.speedMbps != 0) {
                iv->linkSpeedMbps  = ls.speedMbps;
                iv->linkFullDuplex = ls.fullDuplex;
            }
            (void)yt_sync_mac_speed_shared(iv);
            reportLink(true);
            return;
        }
        if (now - iv->loopbackProbeStart >= 8000000000ull) {
            /* Still no cable after 8s of auto-neg: restore loopback. */
            iv->loopbackProbeStart = 0;
            (void)enterPhyLoopback();
        }
        return;
    }

    if (!yt_read_link(iv, &ls)) {
        return;
    }
    if (ls.up == iv->linkUp) {
        return;
    }

    if (ls.up && ls.speedMbps != 0) {
        iv->linkSpeedMbps  = ls.speedMbps;
        iv->linkFullDuplex = ls.fullDuplex;
    }
    if (ls.up) {
        /* Re-program MAC PS/FES from the new negotiation result. */
        (void)yt_sync_mac_speed_shared(iv);
    }
    LOG("link change: link=%u %uM %s", ls.up ? 1u : 0u, ls.speedMbps,
        ls.fullDuplex ? "full duplex" : "half duplex");
    reportLink(ls.up);
}

/* Shared media table for getSupportedMediaArray (stack pull) and
 * ReportAvailableMediaTypes (driver push). Auto-neg first (default). */
static const MediaWord kYTMediaTable[] = {
    (MediaWord)(kIOUserNetworkMediaEthernetAuto),
    (MediaWord)(kIOUserNetworkMediaEthernet1000BaseT | kIOUserNetworkMediaOptionFullDuplex),
    (MediaWord)(kIOUserNetworkMediaEthernet100BaseTX | kIOUserNetworkMediaOptionFullDuplex),
    (MediaWord)(kIOUserNetworkMediaEthernet10BaseT   | kIOUserNetworkMediaOptionFullDuplex),
};
#define YT_MEDIA_TABLE_COUNT (sizeof(kYTMediaTable) / sizeof(kYTMediaTable[0]))

/* ---- PacketPoller: periodic harvest callback ---- */
static IOReturn
yt_poller_action(OSObject * target,
                 IOUserNetworkPacketPoller * poller,
                 void * refCon)
{
    io_miniyv_yt6801ethernet * self =
        OSDynamicCast(io_miniyv_yt6801ethernet, target);
    if (self == nullptr) {
        return kIOReturnSuccess;
    }
    /* Same order as the doorbell path: TX first, then RX. */
    self->reclaimTx();
    self->serviceRx();
    return kIOReturnSuccess;
}

static IOReturn
yt_poller_event(OSObject * target,
                IOUserNetworkPacketPoller * poller,
                IOOptionBits event,
                void * refCon)
{
    LOG("PacketPoller event 0x%x (%{public}s)", event,
        (event & kIOUserNetworkPacketPollerEventPollStart) ? "poll start" :
        (event & kIOUserNetworkPacketPollerEventPollStop)  ? "poll stop" : "other");
    return kIOReturnSuccess;
}


/* ==================================================================
 * Network stack registration.
 * One shared pool, four queues: two submission then two completion.
 * Register via the NDK_24 overload (with MAC address).
 * ================================================================== */
static kern_return_t
yt_register_network_interface(io_miniyv_yt6801ethernet * self, YT * iv)
{
    kern_return_t ret;

    ret = self->CopyDispatchQueue(kIOServiceDefaultQueueName, &iv->dispatchQueue);
    if (ret != kIOReturnSuccess || iv->dispatchQueue == nullptr) {
        LOG("CopyDispatchQueue(Default) failed 0x%x", ret);
        return ret == kIOReturnSuccess ? kIOReturnNoResources : ret;
    }

    {
        IOUserNetworkPacketBufferPoolOptions options = {};
        options.packetCount         = YT6801_POOL_PACKETS;
        options.bufferCount         = YT6801_POOL_PACKETS;
        options.bufferSize          = YT6801_RX_BUFFER_SIZE;
        options.maxBuffersPerPacket = 1;
        /* Both MapToDext (CPU view) and MapToDevice (DMA descriptors) are required. */
        options.poolFlags           = PoolFlagMapToDext
                                    | PoolFlagMapToDevice
                                    | PoolFlagIODirectionIn
                                    | PoolFlagIODirectionOut;
        /* 64 address bits required; anything less breaks the DMA mapping. */
        options.dmaSpecification.options        = 0;
        options.dmaSpecification.maxAddressBits = 64;

        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(
            self, "yt6801-pool", &options, &iv->pool);
        if (ret != kIOReturnSuccess) {
            LOG("packet buffer pool create failed 0x%x", ret);
            return ret;
        }
    }

    ret = IOUserNetworkTxSubmissionQueue::Create(iv->pool, self, YT6801_TX_RING_DESC,
                                                YT6801_QID_TX_SUBMIT,
                                                iv->dispatchQueue, &iv->txSubmit);
    if (ret != kIOReturnSuccess) {
        LOG("TX submission queue create failed 0x%x", ret);
        return ret;
    }

    ret = IOUserNetworkTxCompletionQueue::Create(iv->pool, self, YT6801_TX_RING_DESC,
                                                YT6801_QID_TX_COMPLETE,
                                                iv->dispatchQueue, &iv->txComplete);
    if (ret != kIOReturnSuccess) {
        LOG("TX completion queue create failed 0x%x", ret);
        return ret;
    }

    ret = IOUserNetworkRxSubmissionQueue::Create(iv->pool, self, YT6801_TX_RING_DESC,
                                                YT6801_QID_RX_SUBMIT,
                                                iv->dispatchQueue, &iv->rxSubmit);
    if (ret != kIOReturnSuccess) {
        LOG("RX submission queue create failed 0x%x", ret);
        return ret;
    }

    ret = IOUserNetworkRxCompletionQueue::Create(iv->pool, self, YT6801_TX_RING_DESC,
                                                YT6801_QID_RX_COMPLETE,
                                                iv->dispatchQueue, &iv->rxComplete);
    if (ret != kIOReturnSuccess) {
        LOG("RX completion queue create failed 0x%x", ret);
        return ret;
    }

    {
        IOUserNetworkPacketQueue * queues[4] = {
            iv->txSubmit, iv->rxSubmit, iv->txComplete, iv->rxComplete
        };
        ether_addr_t hwAddr = {};
        for (int i = 0; i < YT6801_MAC_LEN; i++) {
            hwAddr.octet[i] = iv->macAddress[i];
        }

        ret = self->registerEthernetInterface(hwAddr, queues, 4, iv->pool, iv->pool);
        if (ret != kIOReturnSuccess) {
            LOG("registerEthernetInterface failed 0x%x", ret);
            return ret;
        }
        iv->netRegistered = true;
        LOG("network interface registered %02x:%02x:%02x:%02x:%02x:%02x",
            iv->macAddress[0], iv->macAddress[1], iv->macAddress[2],
            iv->macAddress[3], iv->macAddress[4], iv->macAddress[5]);

        /* Push the media table (this stack never pulls getSupportedMediaArray). */
        {
            IOUserNetworkMediaType mediaTypes[YT_MEDIA_TABLE_COUNT];
            for (uint32_t i = 0; i < YT_MEDIA_TABLE_COUNT; i++) {
                mediaTypes[i] = (IOUserNetworkMediaType)kYTMediaTable[i];
            }
            kern_return_t mret =
                self->ReportAvailableMediaTypes(mediaTypes, YT_MEDIA_TABLE_COUNT);
            LOG("pushed media type table %u entries rc=0x%x", (uint32_t)YT_MEDIA_TABLE_COUNT, mret);
        }

        /* PacketPoller: periodic 5ms harvest; doorbell path stays as fallback. */
        if (iv->rxSubmit != nullptr && iv->packetPoller == nullptr) {
            iv->packetPoller = IOUserNetworkPacketPoller::poller(
                self, iv->dispatchQueue,
                yt_poller_action, yt_poller_event, 0, nullptr);
            if (iv->packetPoller != nullptr) {
                IOUserNetworkPacketPollerParameters params = {};
                params.dataRate     = 1000000000ull;   /* 1 Gbps */
                params.pollInterval = 5000000ull;      /* 5 ms */
                kern_return_t pret = iv->packetPoller->setPollerParameters(&params);
                /* Try txSubmit, then rxSubmit. */
                kern_return_t aret = iv->txSubmit != nullptr
                    ? iv->txSubmit->setPacketPoller(iv->packetPoller)
                    : kIOReturnNotReady;
                if (aret != kIOReturnSuccess && iv->rxSubmit != nullptr) {
                    kern_return_t aret2 = iv->rxSubmit->setPacketPoller(iv->packetPoller);
                    LOG("txSubmit attach 0x%x, rxSubmit retry 0x%x", aret, aret2);
                    aret = aret2;
                }
                /* Queue binding may be rejected; enable anyway. */
                if (pret == kIOReturnSuccess) {
                    iv->packetPoller->enable();
                    LOG("PacketPoller enabled (attach=0x%x periodic, 5ms interval)", aret);
                } else {
                    LOG("PacketPoller params failed params=0x%x (doorbell path only)", pret);
                }
            } else {
                LOG("PacketPoller create failed (doorbell path only)");
            }
        }

        /* Fallback harvest timer: IOTimerDispatchSource, 5ms, re-armed in the handler. */
        if (iv->pollTimer == nullptr && iv->dispatchQueue != nullptr) {
            kern_return_t tret = IOTimerDispatchSource::Create(
                iv->dispatchQueue, &iv->pollTimer);
            if (tret == kIOReturnSuccess) {
                OSAction * act = nullptr;
                tret = self->CreateActionPollTimerOccurred(0, &act);
                if (tret == kIOReturnSuccess) {
                    (void)iv->pollTimer->SetHandler(act);
                    OSSafeReleaseNULL(act);
                    (void)iv->pollTimer->SetEnableWithCompletion(true, nullptr);
                    (void)iv->pollTimer->WakeAtTime(
                        kIOTimerClockUptimeRaw,
                        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 5000000ull, 0);
                    LOG("poll timer started (UptimeRaw, 5ms interval)");
                } else {
                    LOG("timer action create failed 0x%x", tret);
                }
            } else {
                LOG("timer create failed 0x%x", tret);
            }
        }
    }

    return kIOReturnSuccess;
}

/* Release in reverse creation order: queues -> pool -> handles. */
static void
yt_release_network_interface(YT * iv)
{
    if (iv->pollTimer != nullptr) {
        (void)iv->pollTimer->SetEnableWithCompletion(false, nullptr);
        OSSafeReleaseNULL(iv->pollTimer);
        LOG("poll timer disabled and released");
    }
    if (iv->packetPoller != nullptr) {
        iv->packetPoller->disable();
        OSSafeReleaseNULL(iv->packetPoller);
        LOG("PacketPoller disabled and released");
    }
    OSSafeReleaseNULL(iv->txSubmit);
    OSSafeReleaseNULL(iv->txComplete);
    OSSafeReleaseNULL(iv->rxSubmit);
    OSSafeReleaseNULL(iv->rxComplete);
    OSSafeReleaseNULL(iv->pool);
    OSSafeReleaseNULL(iv->dispatchQueue);
    iv->netRegistered = false;
}

/* ==================================================================
 * IOService lifecycle
 * ================================================================== */
bool
io_miniyv_yt6801ethernet::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(io_miniyv_yt6801ethernet_IVars, 1);
    if (ivars == nullptr) {
        LOG("ivars allocation failed");
        return false;
    }
    ivars->mtu = YT6801_DEFAULT_MTU;
    LOG("driver instance created");
    return true;
}

void
io_miniyv_yt6801ethernet::free()
{
    IOSafeDeleteNULL(ivars, io_miniyv_yt6801ethernet_IVars, 1);
    super::free();
}

/* ==================================================================
 * LOCALONLY methods: plain virtual functions, implemented directly
 * ================================================================== */
kern_return_t
io_miniyv_yt6801ethernet::readRegister(uint32_t offset, uint32_t * value)
{
    if (ivars == nullptr || !ivars->pciOpened || value == nullptr) {
        return kIOReturnNotReady;
    }
    return yt_read32(ivars, offset, value);
}

kern_return_t
io_miniyv_yt6801ethernet::writeRegister(uint32_t offset, uint32_t value)
{
    if (ivars == nullptr || !ivars->pciOpened) {
        return kIOReturnNotReady;
    }
    return yt_write32(ivars, offset, value);
}

kern_return_t
io_miniyv_yt6801ethernet::mdioRead(uint8_t reg, uint16_t * value)
{
    uint32_t v = 0;
    if (ivars == nullptr || value == nullptr) {
        return kIOReturnBadArgument;
    }
    if (yt_mdio_read(ivars, reg, &v) != 0) {
        return kIOReturnTimeout;
    }
    *value = (uint16_t)v;
    return kIOReturnSuccess;
}

kern_return_t
io_miniyv_yt6801ethernet::mdioWrite(uint8_t reg, uint16_t value)
{
    if (ivars == nullptr) {
        return kIOReturnNotReady;
    }
    return yt_mdio_write(ivars, reg, value) == 0 ? kIOReturnSuccess : kIOReturnTimeout;
}

kern_return_t
io_miniyv_yt6801ethernet::extRead(uint16_t reg, uint16_t * value)
{
    uint32_t v = 0;
    if (ivars == nullptr || value == nullptr) {
        return kIOReturnBadArgument;
    }
    if (yt_ext_read(ivars, reg, &v) != 0) {
        return kIOReturnTimeout;
    }
    *value = (uint16_t)v;
    return kIOReturnSuccess;
}

kern_return_t
io_miniyv_yt6801ethernet::extWrite(uint16_t reg, uint16_t value)
{
    if (ivars == nullptr) {
        return kIOReturnNotReady;
    }
    return yt_ext_write(ivars, reg, value) == 0 ? kIOReturnSuccess : kIOReturnTimeout;
}

kern_return_t
io_miniyv_yt6801ethernet::readMACAddress(uint8_t out[6])
{
    if (ivars == nullptr || out == nullptr) {
        return kIOReturnBadArgument;
    }
    yt_mac_read_address(ivars);
    for (int i = 0; i < YT6801_MAC_LEN; i++) {
        out[i] = ivars->macAddress[i];
    }
    return kIOReturnSuccess;
}

kern_return_t
io_miniyv_yt6801ethernet::dmaSoftReset()
{
    if (ivars == nullptr || !ivars->pciOpened) {
        return kIOReturnNotReady;
    }
    return yt_dma_soft_reset(ivars);
}

kern_return_t
io_miniyv_yt6801ethernet::hardwareInit()
{
    if (ivars == nullptr || !ivars->pciOpened) {
        return kIOReturnNotReady;
    }
    return yt_hardware_init(ivars);
}

kern_return_t
io_miniyv_yt6801ethernet::runLoopbackTest(uint64_t * result)
{
    if (ivars == nullptr || !ivars->pciOpened) {
        if (result) {
            *result = ~0ull;
        }
        return kIOReturnNotReady;
    }
    kern_return_t ret = yt_phy_loopback_test(ivars, &ivars->loopbackResult);
    if (result) {
        *result = ivars->loopbackResult;
    }
    return ret;
}

kern_return_t
io_miniyv_yt6801ethernet::registerInterface()
{
    if (ivars == nullptr || !ivars->hardwareReady) {
        return kIOReturnNotReady;
    }
    return yt_register_network_interface(this, ivars);
}

/* ==================================================================
 * Start / Stop — not LOCALONLY in the .iig, so iig generates the
 * Start_Impl forwarding; implemented with IMPL() here
 * ================================================================== */
kern_return_t
IMPL(io_miniyv_yt6801ethernet, Start)
{
    kern_return_t ret;
    bool     phyLinked = false;     /* link decision result (needed below) */

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        LOG("super::Start failed 0x%x", ret);
        return ret;
    }

    ivars->pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (ivars->pciDevice == nullptr) {
        LOG("provider is not IOPCIDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    ret = ivars->pciDevice->Open(this);
    if (ret != kIOReturnSuccess) {
        LOG("PCI Open failed 0x%x", ret);
        ivars->pciDevice = nullptr;
        Stop(provider, SUPERDISPATCH);
        return ret;
    }
    ivars->pciOpened = true;

    /* Bus master is already enabled by PCIDriverKit for dexts. */

    /* Diagnostic: config space + BAR0; enable Memory Space + Bus Master if off. */
    {
        uint32_t cfg_id = 0, bar0cfg = 0;
        uint16_t cmd = 0, status = 0;

        ivars->pciDevice->ConfigurationRead32(0x00, &cfg_id);
        ivars->pciDevice->ConfigurationRead16(0x04, &cmd);
        ivars->pciDevice->ConfigurationRead32(0x10, &bar0cfg);
        ivars->pciDevice->ConfigurationRead16(0x06, &status);
        LOG("config space: id=0x%08x command=0x%04x status=0x%04x BAR0=0x%08x",
            cfg_id, cmd, status, bar0cfg);

        if ((cmd & 0x0002u) == 0) {
            /* bit1 = Memory Space Enable, bit2 = Bus Master. */
            ivars->pciDevice->ConfigurationWrite16(0x04, (uint16_t)(cmd | 0x0006u));
            ivars->pciDevice->ConfigurationRead16(0x04, &cmd);
            LOG("Memory Space + Bus Master enabled, command now 0x%04x", cmd);
        }
    }

    /* The device may be in D3 (MMIO reads 0xffffffff). D0 pins the
     * function in D0, which is what we want. */
    {
        kern_return_t pr = ivars->pciDevice->EnablePCIPowerManagement(kPCIPMCSPowerStateD0);
        LOG("PCI power state pinned to D0: 0x%x", pr);
    }

    ret = ivars->pciDevice->GetBARInfo(kPCIMemoryRangeBAR0,
                                       &ivars->barIndex,
                                       &ivars->barSize,
                                       &ivars->barType);
    if (ret != kIOReturnSuccess) {
        LOG("GetBARInfo(BAR0) failed 0x%x", ret);
        goto fail_close;
    }
    LOG("PCI match %04x:%04x  BAR0=%llu bytes type=%u memIndex=%u",
        YT6801_VENDOR_ID, YT6801_DEVICE_ID,
        (unsigned long long)ivars->barSize, ivars->barType, ivars->barIndex);

    /* MMIO probe: determines whether all later results can be trusted. */
    {
        uint32_t probe = 0;
        ivars->pciDevice->MemoryRead32(ivars->barIndex,
                                       MAC_VR + YT6801_MAC_REGS_OFFSET, &probe);
        LOG("MMIO probe MAC_VR=0x%08x", probe);
    }

    /* Power-up settle: wait before touching registers. */
    IOSleep(50);

    ret = yt_hardware_init(ivars);
    if (ret != kIOReturnSuccess) {
        LOG("hardware init failed 0x%x", ret);
        goto fail_close;
    }

    yt_mac_read_address(ivars);

    /* Link decision first: the self-test's soft resets tear down an established link. */
    /* ---- PHY line diagnostics: dump both auto-negotiation sides.
     * ANLPAR/STAT1000 show what the partner advertised; LPA=0/garbage
     * means nothing is talking on the wire. ---- */
    {
        uint32_t bmcr = 0, bmsr = 0, anar = 0, lpa = 0;
        uint32_t gbcr = 0, gblsr = 0, spec = 0;
        (void)yt_mdio_read(ivars, REG_MII_BMCR, &bmcr);
        (void)yt_mdio_read(ivars, REG_MII_BMSR, &bmsr);
        (void)yt_mdio_read(ivars, REG_MII_ADVERTISE, &anar);
        (void)yt_mdio_read(ivars, REG_MII_LPA, &lpa);
        (void)yt_mdio_read(ivars, REG_MII_CTRL1000, &gbcr);
        (void)yt_mdio_read(ivars, REG_MII_STAT1000, &gblsr);
        (void)yt_mdio_read(ivars, REG_MII_SPEC_STATUS, &spec);
        LOG("line diag BMCR=0x%04x BMSR=0x%04x advertised ANAR=0x%04x/%04x "
            "partner LPA=0x%04x/%04x SPEC_STATUS=0x%08x",
            bmcr, bmsr, anar, gbcr, lpa, gblsr, spec);
    }

    phyLinked = yt_wait_link(ivars, 30000);
    ivars->loopbackProbeStart = 0;
    ivars->wantLoopback = !phyLinked;
    {
        YTLinkState ls;
        if (yt_read_link(ivars, &ls) && ls.up) {
            ivars->linkSpeedMbps  = ls.speedMbps;
            ivars->linkFullDuplex = ls.fullDuplex;
            LOG("PHY negotiated: %uM %s", ls.speedMbps, ls.fullDuplex ? "full duplex" : "half duplex");
        }
    }
    /* Re-read aneg results after link up. */
    {
        uint32_t bmsr = 0, lpa = 0, gblsr = 0;
        (void)yt_mdio_read(ivars, REG_MII_BMSR, &bmsr);
        (void)yt_mdio_read(ivars, REG_MII_LPA, &lpa);
        (void)yt_mdio_read(ivars, REG_MII_STAT1000, &gblsr);
        LOG("post-aneg BMSR=0x%04x (bit5=aneg done bit2=link) partner LPA=0x%04x STAT1000=0x%04x"
            " (0=silent partner)",
            bmsr, lpa, gblsr);
    }
    /* Key decision log line. */
    LOG("start link decision: %{public}s (wait limit 30s)",
        phyLinked ? "real link, normal datapath"
                  : "no link -> self-loop branch");

    if (!phyLinked) {
        /* No cable: run the control-path self-test. */
        ret = yt_phy_loopback_test(ivars, &ivars->loopbackResult);
        LOG("Loopback test %{public}s (result=0x%llx)",
            ret == kIOReturnSuccess ? "passed" : "failed",
            (unsigned long long)ivars->loopbackResult);
        /* Re-apply analog front-end config (self-test soft resets wipe it). */
        (void)yt_phy_analog_init(ivars);
    }

    /* Register with the network stack; the enX interface appears after
     * this. Failure is not fatal: only the datapath is missing. */
    ret = yt_register_network_interface(this, ivars);
    if (ret != kIOReturnSuccess) {
        LOG("network interface registration failed 0x%x (driver continues without datapath)", ret);
        yt_release_network_interface(ivars);
    } else {
        /* Datapath setup depends on pools/queues created by registration. */
        ret = setupDatapath();
        if (ret != kIOReturnSuccess) {
            LOG("datapath init failed 0x%x (control path still usable)", ret);
        } else {
            /* No cable: self-loop verification mode (PHY digital loopback,
             * link reported UP). */
            if (phyLinked) {
                ivars->selfLoopMode = false;
                /* Real link: open all receive filters (PR|RA). */
                {
                    uint32_t pfr = 0;
                    (void)yt_read32(ivars, MAC_PFR, &pfr);
                    pfr |= (1u << MAC_PFR_PR_POS) | (1u << MAC_PFR_RA_POS);
                    (void)yt_write32(ivars, MAC_PFR, pfr);
                    uint32_t back = 0;
                    (void)yt_read32(ivars, MAC_PFR, &back);
                    LOG("real link: receive filters opened PFR=0x%08x (PR|RA)", back);
                }
                reportLink(true);
            } else if (ivars->loopbackActive) {
                ivars->selfLoopMode = true;
                LOG("no cable: PHY kept in digital loopback, link reported 1000M full duplex (self-loop mode)");
                reportLink(true);
            } else {
                /* No link and no loopback: report DOWN honestly. */
                reportLink(false);
            }
        }
    }

    RegisterService();

    logCounters(1);

    LOG("yt6801 started (version %s)", YT6801_DRV_VERSION);
    return kIOReturnSuccess;

fail_close:
    if (ivars->pciDevice != nullptr) {
        ivars->pciDevice->Close(this);
        ivars->pciDevice = nullptr;
    }
    ivars->pciOpened = false;
    Stop(provider, SUPERDISPATCH);
    return ret;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, Stop)
{
    if (ivars != nullptr) {
        if (ivars->loopbackActive) {
            yt_mdio_write(ivars, REG_MII_BMCR, REG_MII_BMCR_DISABLE_LOOPBACK);
            ivars->loopbackActive = false;
        }
        /* Datapath first (stop DMA, return packets, free rings), then queues and pool. */
        logCounters(2);
        teardownDatapath();
        yt_release_network_interface(ivars);
        if (ivars->pciDevice != nullptr) {
            ivars->pciDevice->Close(this);
            ivars->pciDevice = nullptr;
        }
        ivars->pciOpened = false;
        ivars->hardwareReady = false;
    }
    LOG("yt6801 stopped");
    return Stop(provider, SUPERDISPATCH);
}

/* ==================================================================
 * IOUserNetworkEthernet's 10 deprecated pure-virtual methods.
 * Minimal but honest implementations.
 * ================================================================== */
kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetInterfaceEnable)
{
    ivars->interfaceEnabled = isEnable;

    /* All four queues need explicit setEnable, or the stack delivers nothing. */
    {
        struct { IOUserNetworkPacketQueue * q; const char * name; } qs[4] = {
            { ivars->txSubmit,   "TX submit" },
            { ivars->txComplete, "TX complete" },
            { ivars->rxSubmit,   "RX submit" },
            { ivars->rxComplete, "RX complete" },
        };
        for (int i = 0; i < 4; i++) {
            if (qs[i].q == nullptr) {
                continue;
            }
            kern_return_t qret = qs[i].q->setEnable(isEnable);
            if (qret != kIOReturnSuccess) {
                LOG("%s queue %s failed 0x%x", qs[i].name, isEnable ? "enable" : "disable", qret);
            }
        }
        LOG("all four queues %s", isEnable ? "enabled" : "disabled");
    }

    if (ivars->datapathReady) {
        /* The real TX/RX switch is DMA and MAC_CR, not just a flag. */
        enableDatapath(isEnable);
    } else if (ivars->hardwareReady) {
        yt_set_bits(ivars, MAC_CR, (1u << MAC_CR_RE_POS), isEnable);
        yt_set_bits(ivars, MAC_CR, (1u << MAC_CR_TE_POS), isEnable);
    }

    /* Re-report link: ifnet is now fully attached, active media is accepted. */
    if (ivars->netRegistered) {
        if (isEnable) {
            reportLink(true);
        } else {
            reportLink(false);
        }
    }

    LOG("interface %s | irq=%u txHead=%u txClean=%u txFrames=%u rxFrames=%u doorbell=%u",
        isEnable ? "enabled" : "disabled", ivars->irqCount,
        ivars->txHead, ivars->txClean, ivars->txFrames,
        ivars->rxFrameCount, ivars->doorbellCount);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetPromiscuousModeEnable)
{
    LOG("promiscuous mode %s", enable ? "on" : "off");
    if (!ivars->hardwareReady) {
        return kIOReturnNotReady;
    }
    return yt_set_bits(ivars, MAC_PFR, (1u << MAC_PFR_PR_POS), enable);
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetAllMulticastModeEnable)
{
    LOG("all-multicast mode %s", enable ? "on" : "off");
    if (!ivars->hardwareReady) {
        return kIOReturnNotReady;
    }
    return yt_set_bits(ivars, MAC_PFR, (1u << MAC_PFR_PM_POS), enable);
}

/* ==================================================================
 * Multicast hash table (MAC_HTR0..7, 64 bits = 256 buckets):
 *   1. standard CRC-32 of the 6-byte address (poly 0xEDB88320, init
 *      all 1s, final inversion)
 *   2. bit-reverse the 32-bit result
 *   3. high 8 bits: top 3 select the register (of 8), low 5 the bit
 * Required for IPv6: neighbor discovery etc. uses 33:33 multicast.
 * ================================================================== */
static uint32_t
yt_crc32_le(const uint8_t * data, int length)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (--length >= 0) {
        uint8_t byte = *data++;
        for (int bit = 8; --bit >= 0; byte >>= 1) {
            if ((crc ^ byte) & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void
yt_mcast_hash_bit(YT * iv, const uint8_t mac[YT6801_MAC_LEN], bool add)
{
    uint32_t crc = yt_crc32_le(mac, YT6801_MAC_LEN);
    uint32_t rev = 0;

    for (int j = 0; j < 32; j++) {
        if (crc & (1u << j)) {
            rev |= (1u << (31 - j));
        }
    }

    uint32_t reg = (rev >> 5) & 0x7u;
    uint32_t bit = rev & 0x1Fu;
    uint32_t v = 0;

    yt_read32(iv, MAC_HTR0 + reg * MAC_HTR_INC, &v);
    if (add) {
        v |= (1u << bit);
    } else {
        v &= ~(1u << bit);
    }
    yt_write32(iv, MAC_HTR0 + reg * MAC_HTR_INC, v);
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetMulticastAddresses)
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->hardwareReady) {
        return kIOReturnNotReady;
    }

    /* Clear the whole table then refill: the stack gives a full list. */
    for (uint32_t r = 0; r < 8; r++) {
        yt_write32(iv, MAC_HTR0 + r * MAC_HTR_INC, 0);
    }

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (addresses == nullptr) {
            break;
        }
        yt_mcast_hash_bit(iv, addresses[i].octet, true);
        n++;
    }

    /* Enable hash multicast; disable pass-all-multicast or the table is
     * pointless. */
    uint32_t pfr = 0;
    yt_read32(iv, MAC_PFR, &pfr);
    pfr |= (1u << MAC_PFR_HMC_POS);
    yt_write32(iv, MAC_PFR, pfr);

    LOG("multicast hash table written with %u addresses PFR=0x%08x", n, pfr);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SelectMediaType)
{
    (void)mediaType;
    return kIOReturnSuccess;
}

/* Supported media list (NDK_21 getSupportedMediaArray). Order matters:
 * the first entry is the default, so auto-neg goes first. */
kern_return_t
io_miniyv_yt6801ethernet::getSupportedMediaArray(
    MediaWord * mediaArray, uint32_t * mediaCount)
{
    /* Shared table, see kYTMediaTable above. */
    const uint32_t n = (uint32_t)YT_MEDIA_TABLE_COUNT;

    if (mediaArray == nullptr || mediaCount == nullptr) {
        return kIOReturnBadArgument;
    }

    /* The caller's array is sized by *mediaCount; on insufficient
     * capacity, return the needed count in *mediaCount. */
    if (*mediaCount < n) {
        *mediaCount = n;
        return kIOReturnNoSpace;
    }

    for (uint32_t i = 0; i < n; i++) {
        mediaArray[i] = kYTMediaTable[i];
    }
    *mediaCount = n;

    LOG("media type table %u entries", n);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetWakeOnMagicPacketEnable)
{
    LOG("wake on magic packet %s", enable ? "on" : "off");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetMTU)
{
    if (mtu < 68 || mtu > YT6801_MAX_PACKET_SIZE) {
        return kIOReturnBadArgument;
    }
    ivars->mtu = mtu;
    LOG("MTU set to %u", mtu);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, GetMaxTransferUnit)
{
    if (mtu == nullptr) {
        return kIOReturnBadArgument;
    }
    *mtu = ivars->mtu;
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, SetHardwareAssists)
{
    /* No offloads implemented: checksums, TSO, VLAN all in software.
     * Lying here produces corrupt packets. */
    (void)hardwareAssists;
    return kIOReturnSuccess;
}

kern_return_t
IMPL(io_miniyv_yt6801ethernet, GetHardwareAssists)
{
    if (hardwareAssists == nullptr) {
        return kIOReturnBadArgument;
    }
    *hardwareAssists = 0;
    return kIOReturnSuccess;
}
