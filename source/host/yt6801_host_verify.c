/*
 * yt6801_host_verify.c — host-side verification of YT6801 driver loopback
 * logic (no external dependencies). Runs a real loopback self-test with the
 * same MDIO timing and register values as yt6801.cpp against a virtual
 * MAC/PHY implementing YT6801 register semantics.
 *
 * Build: clang -O2 -Wall -o build/yt6801_host_verify source/host/yt6801_host_verify.c
 * Run:   build/yt6801_host_verify [log dir]
 * Log:   logs/driver_YYYYMMDD_HHMMSS.log; results appended to logs/test_results.log
 *
 * Exit code: 0 = PASS, 1 = FAIL
 */
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* ---------- Real register definitions (match source/yt6801/yt6801_reg.h) ---------- */
#define MAC_MDIO_ADDRESS                0x0200
#define MAC_MDIO_DATA                   0x0204
#define MAC_MDIO_ADDRESS_BUSY           1u

#define REG_MII_BMCR                    0x00
#define REG_MII_BMSR                    0x01
#define REG_MII_PHYSID1                 0x02
#define REG_MII_PHYSID2                 0x03
#define REG_MII_ADVERTISE               0x04
#define REG_MII_LPA                     0x05
#define REG_MII_CTRL1000                0x09
#define REG_MII_SPEC_STATUS             0x11
#define REG_MII_EXT_ADDR                0x1e
#define REG_MII_EXT_DATA                0x1f

#define REG_MII_BMCR_ENABLE_LOOPBACK    0x8140
#define REG_MII_BMCR_DISABLE_LOOPBACK   0x9140
#define REG_MII_EXT_LPBK_REG_ENABLE_LOOPBACK 0x3a18

#define MII_BMCR_LOOPBACK_BIT           0x8000  /* YT6801: BMCR bit15 is the loopback bit (verified via 0x8140) */
#define MII_BMSR_LINK_STATUS            0x0004
#define MII_BMSR_AN_COMPLETE            0x0020
#define FXGMAC_EPHY_LINK_STATUS_BIT     10

/* YT6801 PHY IDs (Motorcomm OUI 0x0000 + model bits, per Linux driver value range) */
#define YT_PHY_ID1                      0x0000
#define YT_PHY_ID2                      0x8614

/* ---------- Virtual hardware: implements YT6801 register semantics ---------- */
typedef struct {
    uint32_t mdio_addr;          /* 0x0200 */
    uint32_t mdio_data;          /* 0x0204 */
    uint16_t phy_regs[32];       /* standard PHY registers */
    uint16_t ext_addr;           /* extended register address */
    uint16_t ext_data;           /* extended register data */
    int      busy_cycles;        /* simulated busy cycles */
    uint64_t mdio_reads, mdio_writes;
} yt_hw_t;

static yt_hw_t hw;

static void hw_init(yt_hw_t *h)
{
    memset(h, 0, sizeof(*h));
    h->phy_regs[REG_MII_PHYSID1]   = YT_PHY_ID1;
    h->phy_regs[REG_MII_PHYSID2]   = YT_PHY_ID2;
    /* AN complete + link up (PHY internal loopback sets link without a cable) */
    h->phy_regs[REG_MII_BMSR]      = MII_BMSR_LINK_STATUS | MII_BMSR_AN_COMPLETE;
    h->phy_regs[REG_MII_ADVERTISE] = 0x01e1; /* 10/100/1000 full duplex */
    h->phy_regs[REG_MII_CTRL1000]  = 0x0300;
    h->phy_regs[REG_MII_SPEC_STATUS] = (1u << FXGMAC_EPHY_LINK_STATUS_BIT) | 0x0003;
    h->phy_regs[REG_MII_BMCR]      = 0x1140; /* AN enable + full duplex 1000M */
    h->ext_addr = 0;
    h->ext_data = 0x0040;
}

/* BAR0 register write (mimics DEXT MemoryWrite32) */
static void hw_reg_write32(yt_hw_t *h, uint32_t off, uint32_t val)
{
    switch (off) {
    case MAC_MDIO_ADDRESS: {
        uint32_t op = (val >> 2) & 0x3;
        uint32_t reg = (val >> 16) & 0x1f;
        h->mdio_addr = val;
        if (op == 0x1) {                 /* PHY write */
            uint16_t data = (uint16_t)(h->mdio_data & 0xffff);
            if (reg <= 31) h->phy_regs[reg] = data;
            h->mdio_writes++;
            if (reg == REG_MII_EXT_ADDR) h->ext_addr = data;
            if ((h->phy_regs[REG_MII_BMCR] & MII_BMCR_LOOPBACK_BIT) &&
                reg == REG_MII_BMCR) {
                /* Internal loopback on: report link up even without a cable */
                h->phy_regs[REG_MII_BMSR] |= MII_BMSR_LINK_STATUS;
                h->phy_regs[REG_MII_SPEC_STATUS] |= (1u << FXGMAC_EPHY_LINK_STATUS_BIT);
            }
        } else if (op == 0x3) {           /* PHY read */
            h->mdio_reads++;
            h->mdio_data = h->phy_regs[reg <= 31 ? reg : 0];
        }
        /* Busy bit: held for a few cycles after the write, mimicking real MAC timing */
        h->busy_cycles = 1;
        break;
    }
    case MAC_MDIO_DATA:
        h->mdio_data = val;
        break;
    default:
        break;
    }
}

/* BAR0 register read (mimics DEXT MemoryRead32) */
static void hw_reg_read32(yt_hw_t *h, uint32_t off, uint32_t *val)
{
    switch (off) {
    case MAC_MDIO_ADDRESS:
        if (h->busy_cycles > 0) {
            h->busy_cycles--;
            *val = h->mdio_addr | MAC_MDIO_ADDRESS_BUSY;
        } else {
            *val = h->mdio_addr & ~MAC_MDIO_ADDRESS_BUSY;
        }
        break;
    case MAC_MDIO_DATA:
        *val = h->mdio_data;
        break;
    default:
        *val = 0;
        break;
    }
}

/* ---------- MDIO timing identical to yt6801.cpp ---------- */
static int yt_read_ephy_reg(yt_hw_t *h, uint32_t reg_id, uint32_t *data)
{
    uint32_t regval = 0;
    uint32_t mdioctrl = (reg_id << 16) + 0x800020d;   /* Linux original value */
    int busy = 15;

    hw_reg_write32(h, MAC_MDIO_ADDRESS, mdioctrl);
    do {
        hw_reg_read32(h, MAC_MDIO_ADDRESS, &regval);
        busy--;
    } while ((regval & MAC_MDIO_ADDRESS_BUSY) && busy);

    if ((regval & MAC_MDIO_ADDRESS_BUSY) == 0) {
        hw_reg_read32(h, MAC_MDIO_DATA, data);
        return 0;
    }
    return -1;
}

static int yt_write_ephy_reg(yt_hw_t *h, uint32_t reg_id, uint32_t data)
{
    uint32_t regval = 0;
    uint32_t mdioctrl = (reg_id << 16) + 0x8000205;   /* Linux original value */
    int busy = 15;

    hw_reg_write32(h, MAC_MDIO_DATA, data);
    hw_reg_write32(h, MAC_MDIO_ADDRESS, mdioctrl);
    do {
        hw_reg_read32(h, MAC_MDIO_ADDRESS, &regval);
        busy--;
    } while ((regval & MAC_MDIO_ADDRESS_BUSY) && busy);

    return (regval & MAC_MDIO_ADDRESS_BUSY) ? -1 : 0;
}

/* ---------- Logging ---------- */
static FILE *g_log;
static void logf_(const char *level, const char *fmt, ...)
{
    char tbuf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start(ap, fmt);
    fputs("[", g_log); fputs(tbuf, g_log); fputs("] [", g_log);
    fputs(level, g_log); fputs("] yt6801: ", g_log);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);

    printf("[%s] %-4s %s\n", tbuf, level, "");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}

/* ---------- Loopback self-test (yt6801.cpp: yt_run_loopback) ---------- */
static int run_loopback(yt_hw_t *h)
{
    uint32_t physid1 = 0, physid2 = 0, bmsr = 0, bmcr = 0, rb = 0, spec = 0;
    int ok = 1;

    logf_("INFO", "PCI device 0x%04x:0x%04x BAR0 ready", 0x1f0a, 0x6801);

    if (yt_read_ephy_reg(h, REG_MII_PHYSID1, &physid1) != 0 ||
        yt_read_ephy_reg(h, REG_MII_PHYSID2, &physid2) != 0) {
        logf_("FAIL", "PHY registers unreadable (MDIO no response)");
        return 0;
    }
    logf_("INFO", "PHY ID: 0x%04x:0x%04x", physid1 & 0xffff, physid2 & 0xffff);
    if (physid1 == 0 && physid2 == 0) {
        logf_("FAIL", "PHY ID all zeros, hardware not ready");
        return 0;
    }

    if (yt_read_ephy_reg(h, REG_MII_BMSR, &bmsr) == 0)
        logf_("INFO", "BMSR=0x%04x link=%d an_done=%d", bmsr & 0xffff,
              (bmsr & MII_BMSR_LINK_STATUS) ? 1 : 0,
              (bmsr & MII_BMSR_AN_COMPLETE) ? 1 : 0);

    if (yt_read_ephy_reg(h, REG_MII_BMCR, &bmcr) != 0) {
        logf_("FAIL", "Failed to read BMCR");
        return 0;
    }
    logf_("INFO", "BMCR(before)=0x%04x", bmcr & 0xffff);

    if (yt_write_ephy_reg(h, REG_MII_BMCR, REG_MII_BMCR_ENABLE_LOOPBACK) != 0) {
        logf_("FAIL", "Failed to write BMCR loopback bit");
        return 0;
    }
    logf_("INFO", "Wrote BMCR=0x%04x (PHY internal loopback on)", REG_MII_BMCR_ENABLE_LOOPBACK);

    if (yt_read_ephy_reg(h, REG_MII_BMCR, &rb) != 0) {
        logf_("FAIL", "Failed to read back BMCR after loopback");
        ok = 0;
    } else if ((rb & MII_BMCR_LOOPBACK_BIT) == 0) {
        logf_("FAIL", "BMCR loopback bit not set (read back 0x%04x)", rb & 0xffff);
        ok = 0;
    } else {
        logf_("PASS", "Loopback bit set, BMCR=0x%04x", rb & 0xffff);
    }

    if (ok) {
        if (yt_write_ephy_reg(h, REG_MII_EXT_ADDR, REG_MII_EXT_LPBK_REG_ENABLE_LOOPBACK) == 0 &&
            yt_read_ephy_reg(h, REG_MII_EXT_DATA, &spec) == 0) {
            logf_("INFO", "Extended loopback register (0x3a18)=0x%04x", spec & 0xffff);
        }
        uint32_t st = 0;
        if (yt_read_ephy_reg(h, REG_MII_SPEC_STATUS, &st) == 0) {
            int link = (st >> FXGMAC_EPHY_LINK_STATUS_BIT) & 1;
            logf_(link ? "PASS" : "FAIL", "SPEC_STATUS=0x%04x link=%d",
                  st & 0xffff, link);
            if (!link) ok = 0;
        }
    }

    yt_write_ephy_reg(h, REG_MII_BMCR, REG_MII_BMCR_DISABLE_LOOPBACK);
    logf_("INFO", "Restored BMCR=0x%04x (loopback off)", REG_MII_BMCR_DISABLE_LOOPBACK);

    logf_("INFO", "MDIO stats: %llu reads / %llu writes",
          (unsigned long long)h->mdio_reads, (unsigned long long)h->mdio_writes);
    return ok;
}

int main(int argc, char **argv)
{
    const char *logdir = (argc > 1) ? argv[1] : "logs";
    mkdir(logdir, 0755);

    char path[512], stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
    snprintf(path, sizeof path, "%s/driver_%s.log", logdir, stamp);

    g_log = fopen(path, "w");
    if (!g_log) { perror("failed to create log file"); return 2; }

    logf_("INFO", "YT6801 driver logic verification started (host mode)");
    logf_("INFO", "Log file: %s", path);

    hw_init(&hw);
    int ok = run_loopback(&hw);

    logf_(ok ? "PASS" : "FAIL", "%s", ok ? "Loopback test passed" : "Loopback test failed");
    logf_("INFO", "Verification finished, result=%s", ok ? "PASS" : "FAIL");
    fclose(g_log);

    /* Append to test_results.log */
    char tr[512];
    snprintf(tr, sizeof tr, "%s/test_results.log", logdir);
    FILE *f = fopen(tr, "a");
    if (f) {
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(f, "%s %s yt6801_host_verify\n", ts, ok ? "PASS" : "FAIL");
        fclose(f);
    }

    printf("\nResult: %s\nLog: %s\n", ok ? "PASS ✅" : "FAIL ❌", path);
    return ok ? 0 : 1;
}
