/*
 * yt6801_datapath.cpp — TX/RX descriptor rings, interrupts and datapath.
 * All entry points (interrupts, doorbells, ifconfig) land on one dispatch
 * queue and are serialized; no locks needed in this file.
 */
#include <os/log.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODataQueueDispatchSource.h>

#include <PCIDriverKit/PCIDriverKit.h>

#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacket.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>

#include <io.miniyv.yt6801ethernet/io_miniyv_yt6801ethernet.h>
#include "yt6801_ivars.h"

/* DriverKit's os/log.h has no os_log_create, only OS_LOG_DEFAULT. */
#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "yt6801: " fmt, ##__VA_ARGS__)

typedef io_miniyv_yt6801ethernet_IVars YT;

/* ==================================================================
 * Register access (same shape as yt6801.cpp, whose copies are static)
 * ================================================================== */
static inline uint32_t
reg_read(YT * iv, uint32_t off)
{
    uint32_t v = 0;
    iv->pciDevice->MemoryRead32(iv->barIndex, off + YT6801_MAC_REGS_OFFSET, &v);
    return v;
}

static inline void
reg_write(YT * iv, uint32_t off, uint32_t v)
{
    iv->pciDevice->MemoryWrite32(iv->barIndex, off + YT6801_MAC_REGS_OFFSET, v);
}

static inline uint32_t
dma_ch_reg(uint32_t off)
{
    return DMA_CH_BASE + off;
}

static inline uint32_t
mtl_q_reg(uint32_t q, uint32_t off)
{
    return MTL_Q_BASE + q * MTL_Q_INC + off;
}

/* ==================================================================
 * Descriptor rings
 *
 * Each descriptor is 16 bytes, four little-endian u32:
 *   desc0/1 = buffer address low/high 32 bits
 *   desc2   = length/control
 *   desc3   = OWN and other status bits
 * ================================================================== */
static inline volatile uint32_t *
desc_at(volatile uint32_t * ring, uint32_t i)
{
    return (volatile uint32_t *)((uintptr_t)ring + (size_t)i * YT6801_DESC_SIZE);
}

static inline uint32_t
ring_next(uint32_t i, uint32_t count)
{
    return (i + 1) & (count - 1);   /* count is a power of two */
}

static inline uint32_t
ring_prev(uint32_t i, uint32_t count)
{
    return (i + count - 1) & (count - 1);
}

/*
 * Allocate device-visible, CPU-writable memory and provide three things:
 * CPU virtual address (write descriptors), device physical address
 * (programmed into TDTR/RDTR), and the DMA command.
 *
 * PrepareForDMA must yield a single segment: the descriptor ring is
 * traversed sequentially by hardware; multiple segments would produce
 * wrong next-descriptor addresses. Fail loudly at init rather than
 * corrupt memory later.
 */
static kern_return_t
alloc_dma_ring(io_miniyv_yt6801ethernet * self,
               YT * iv,
               uint32_t bytes,
               IOBufferMemoryDescriptor ** memOut,
               IOMemoryMap ** mapOut,
               IODMACommand ** dmaOut,
               volatile uint32_t ** descOut,
               uint64_t * physOut)
{
    IOBufferMemoryDescriptor * mem = nullptr;
    IOMemoryMap *              map = nullptr;
    IODMACommand *             dma = nullptr;
    kern_return_t              ret;

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bytes, 4096, &mem);
    if (ret != kIOReturnSuccess || mem == nullptr) {
        LOG("descriptor ring alloc failed 0x%x (%u bytes)", ret, bytes);
        return ret == kIOReturnSuccess ? kIOReturnNoMemory : ret;
    }

    ret = mem->CreateMapping(0, 0, 0, bytes, 0, &map);
    if (ret != kIOReturnSuccess || map == nullptr) {
        LOG("descriptor ring map failed 0x%x", ret);
        OSSafeReleaseNULL(mem);
        return ret == kIOReturnSuccess ? kIOReturnNoMemory : ret;
    }

    {
        IODMACommandSpecification spec = {};
        spec.options        = 0;
        spec.maxAddressBits = 64;       /* YT6801 is a 64-bit DMA master */

        ret = IODMACommand::Create(iv->pciDevice, kIODMACommandCreateNoOptions, &spec, &dma);
        if (ret != kIOReturnSuccess || dma == nullptr) {
            LOG("IODMACommand create failed 0x%x", ret);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(mem);
            return ret == kIOReturnSuccess ? kIOReturnNoMemory : ret;
        }
    }

    {
        uint64_t        flags = 0;
        uint32_t        segCount = 32;
        IOAddressSegment segs[32] = {};
        uint32_t        want = bytes;

        ret = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                 mem, 0, 0, &flags, &segCount, segs);
        if (ret != kIOReturnSuccess) {
            LOG("PrepareForDMA failed 0x%x", ret);
            OSSafeReleaseNULL(dma);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(mem);
            return ret;
        }
        if (segCount != 1) {
            LOG("descriptor ring not physically contiguous (%u segments), aborting", segCount);
            dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            OSSafeReleaseNULL(dma);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(mem);
            return kIOReturnNoResources;
        }
        if (segs[0].length < want) {
            LOG("DMA segment length %llu < required %u",
                (unsigned long long)segs[0].length, want);
            dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            OSSafeReleaseNULL(dma);
            OSSafeReleaseNULL(map);
            OSSafeReleaseNULL(mem);
            return kIOReturnNoResources;
        }
        *physOut = segs[0].address;
    }

    *memOut  = mem;
    *mapOut  = map;
    *dmaOut  = dma;
    *descOut = (volatile uint32_t *)(uintptr_t)map->GetAddress();

    return kIOReturnSuccess;
}

static void
free_dma_ring(IOBufferMemoryDescriptor ** mem, IOMemoryMap ** map, IODMACommand ** dma)
{
    if (*dma != nullptr) {
        (*dma)->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    OSSafeReleaseNULL(*dma);
    OSSafeReleaseNULL(*map);
    OSSafeReleaseNULL(*mem);
}

/* ==================================================================
 * Returning packets to the stack
 * ================================================================== */

/* TX: packet sent, return it on the TX completion queue so the stack
 * can recycle the buffer. */
static void
tx_return_packet(YT * iv, IOUserNetworkPacket * pkt)
{
    if (pkt == nullptr) {
        return;
    }
    if (iv->txComplete == nullptr) {
        iv->pool->deallocatePacket(pkt);
        return;
    }
    IOReturn ret = iv->txComplete->enqueuePackets(&pkt, 1);
    if (ret != kIOReturnSuccess) {
        /* Queue gone: fall back to the pool, at least no leak. */
        LOG("TX completion queue return failed 0x%x, back to pool", ret);
        iv->pool->deallocatePacket(pkt);
    }
}

/* RX: frame received, hand it to the stack. */
static IOReturn
rx_deliver_packet(YT * iv, IOUserNetworkPacket * pkt)
{
    if (pkt == nullptr) {
        return kIOReturnBadArgument;
    }
    if (iv->rxComplete == nullptr) {
        return kIOReturnNotReady;
    }
    return iv->rxComplete->enqueuePackets(&pkt, 1);
}

/* Put a harvested packet back into rxFree for the next refill. If
 * rxFree is full the ring is at capacity and the packet goes back to
 * the pool. */
static void
rx_recycle_packet(YT * iv, IOUserNetworkPacket * pkt)
{
    if (pkt == nullptr) {
        return;
    }
    if (iv->rxFreeCount < YT6801_RX_RING_DESC) {
        iv->rxFree[iv->rxFreeCount++] = pkt;
        return;
    }
    if (iv->pool != nullptr) {
        iv->pool->deallocatePacket(pkt);
    }
}

/* ==================================================================
 * RX ring
 * ================================================================== */

/* Freshly allocated pool packets must be prepareWithQueue'd to get a
 * DMA address the hardware can actually use. Packets returned by the
 * stack via rxSubmit are already prepared; only the 128 bare pool
 * allocations need this. */
static kern_return_t
rx_prepare_packet(YT * iv, IOUserNetworkPacket * pkt)
{
    if (pkt == nullptr) {
        return kIOReturnBadArgument;
    }
    if (iv->rxSubmit == nullptr) {
        return kIOReturnNotReady;
    }
    return pkt->prepareWithQueue(iv->rxSubmit, kIOUserNetworkPacketDirectionRx);
}

/*
 * Arm descriptor i with a packet and hand OWN to the hardware.
 *
 * Order: write address and BUF1V, barrier, set OWN last — otherwise
 * the hardware may read a half-written descriptor.
 */
static void
rx_arm_descriptor(YT * iv, uint32_t i, IOUserNetworkPacket * pkt)
{
    /* Descriptor points at the driver-allocated slot buffer;
     * copy into the pool packet after RX. */
    uint64_t buf = (iv->rxSlotPhys[i] != 0)
                 ? iv->rxSlotPhys[i]
                 : (uint64_t)pkt->getDataIOVirtualAddress();
    volatile uint32_t * d = desc_at(iv->rxDesc, i);

    d[0] = (uint32_t)(buf & 0xFFFFFFFFull);
    /* RDES1 = buffer address high 32 bits (same 64-bit split as TDES1). */
    d[1] = (uint32_t)(buf >> 32);
    /* RDES2 bits 0..13 = B1L, buffer 1 length (must match the buffer size). */
    d[2] = (uint32_t)(YT6801_RX_BUFFER_SIZE & 0x3FFFu);
    /* INTE = interrupt per frame; BUF1V = buffer 1 valid */
    d[3] = (1u << RDESC3_INTE_POS) | (1u << RDESC3_BUF1V_POS);

    OSSynchronizeIO();

    d[3] |= (1u << RDESC3_OWN_POS);
    OSSynchronizeIO();

    iv->rxPackets[i] = pkt;
}

/* RDTR must hold the descriptor just armed (inclusive tail), matching
 * the vendor driver's "last cleaned entry". The ring is page-aligned so
 * the low 32 bits are enough; the high bits come from RDLR_HI. */
static void
pushRxTail(YT * iv)
{
    uint32_t last = ring_prev(iv->rxArm, YT6801_RX_RING_DESC);
    uint64_t tail = iv->rxRingPhys + (uint64_t)last * YT6801_DESC_SIZE;
    reg_write(iv, dma_ch_reg(DMA_CH_RDTR_LO), (uint32_t)(tail & 0xFFFFFFFFull));
}

/* Refill empty ring slots from rxFree.
 * Full/empty is tracked with the explicit rxPending counter. */
void
io_miniyv_yt6801ethernet::refillRx()
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady) {
        return;
    }

    uint32_t armed = 0;

    /* Keep one slot free: a full ring makes the tail land behind CARDR
     * (tail = rxArm-1 = CARDR-1), the hardware stops for good and no
     * buffer ever comes back. */
    while (iv->rxFreeCount > 0 && iv->rxPending < YT6801_RX_RING_DESC - 1) {
        IOUserNetworkPacket * pkt = iv->rxFree[--iv->rxFreeCount];
        rx_arm_descriptor(iv, iv->rxArm, pkt);
        iv->rxArm = ring_next(iv->rxArm, YT6801_RX_RING_DESC);
        iv->rxPending++;
        armed++;
    }

    /* RDTR is the last descriptor handed over, not the next free one:
     * writing rxArm makes the hardware stop one slot early every time
     * (CARDR ends up RDTR+16, SR reports RPS). */
    if (armed != 0) {
        pushRxTail(iv);
    }
}

/* Harvest frames whose OWN bit the hardware has cleared and hand them
 * to the stack. */
void
io_miniyv_yt6801ethernet::serviceRx()
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady) {
        return;
    }

    uint32_t harvested = 0;

    while (iv->rxPending > 0) {
        volatile uint32_t * d = desc_at(iv->rxDesc, iv->rxClean);
        uint32_t d3 = d[3];

        if (d3 & (1u << RDESC3_OWN_POS)) {
            break;                      /* hardware not done yet */
        }

        uint32_t plen = d3 & RDESC3_PL_MASK;
        bool     err  = (d3 & (1u << RDESC3_ES_POS)) != 0;
        uint32_t slot = iv->rxClean;
        IOUserNetworkPacket * pkt = iv->rxPackets[slot];
        iv->rxPackets[slot] = nullptr;

        /* All reject cases advance d[3], rxPending and rxClean together;
         * skipping one desynchronizes the whole ring permanently. */
        uint32_t dropWhy = 0;           /* 1=no pkt 2=hw error 3=short 4=oversize */

        if (pkt == nullptr) {
            dropWhy = 1;
        } else if (err) {
            dropWhy = 2;
        } else if (plen < YT6801_RX_MIN_FRAME) {
            /* Ghost frame: OWN + PL set but no data moved; reject. */
            dropWhy = 3;
        } else if (plen > YT6801_RX_BUFFER_SIZE) {
            dropWhy = 4;                /* descriptor was clobbered */
        }

        if (dropWhy != 0) {
            iv->rxErrors++;

            if (dropWhy == 3) {
                iv->rxFakeFrames++;
                if (iv->rxFakeLogged < 4) {
                    const uint8_t * s = (iv->rxSlotMap[slot] != nullptr)
                        ? (const uint8_t *)(uintptr_t)iv->rxSlotMap[slot]->GetAddress()
                        : nullptr;
                    LOG("drop ghost frame #%u: slot %u PL=%u desc=[%08x %08x %08x %08x] buf=%02x %02x %02x %02x %02x %02x %02x %02x",
                        iv->rxFakeFrames, slot, plen, d[0], d[1], d[2], d3,
                        s != nullptr ? s[0] : 0, s != nullptr ? s[1] : 0,
                        s != nullptr ? s[2] : 0, s != nullptr ? s[3] : 0,
                        s != nullptr ? s[4] : 0, s != nullptr ? s[5] : 0,
                        s != nullptr ? s[6] : 0, s != nullptr ? s[7] : 0);
                    iv->rxFakeLogged++;
                }
            }

            rx_recycle_packet(iv, pkt);
            d[3] = 0;
            iv->rxPending--;
            iv->rxClean = ring_next(slot, YT6801_RX_RING_DESC);
            continue;
        }

        /* The frame was DMA'd into the driver buffer; copy it into the
         * pool packet before handing it to the stack. */
        if (iv->rxSlotMap[slot] != nullptr) {
            const uint8_t * src = (const uint8_t *)(uintptr_t)
                                  iv->rxSlotMap[slot]->GetAddress();
            uint8_t * dst = (uint8_t *)(uintptr_t)pkt->getDataVirtualAddress();
            uint32_t n = plen < YT6801_RX_BUFFER_SIZE ? plen : YT6801_RX_BUFFER_SIZE;
            for (uint32_t k = 0; k < n; k++) {
                dst[k] = src[k];
            }
        }

        /* Validate the Ethernet header after the copy (EtherType 0 = stale buffer). */
        {
            const uint8_t * b = (const uint8_t *)(uintptr_t)pkt->getDataVirtualAddress();
            uint16_t et = (b != nullptr) ? (uint16_t)(((uint16_t)b[12] << 8) | b[13]) : 0;

            if (et == 0) {
                iv->rxErrors++;
                iv->rxFakeFrames++;
                if (iv->rxFakeLogged < 4) {
                    LOG("drop header-less frame #%u: slot %u PL=%u first8=%02x %02x %02x %02x %02x %02x %02x %02x",
                        iv->rxFakeFrames, slot, plen,
                        b != nullptr ? b[0] : 0, b != nullptr ? b[1] : 0,
                        b != nullptr ? b[2] : 0, b != nullptr ? b[3] : 0,
                        b != nullptr ? b[4] : 0, b != nullptr ? b[5] : 0,
                        b != nullptr ? b[6] : 0, b != nullptr ? b[7] : 0);
                    iv->rxFakeLogged++;
                }
                rx_recycle_packet(iv, pkt);
                d[3] = 0;
                iv->rxPending--;
                iv->rxClean = ring_next(slot, YT6801_RX_RING_DESC);
                continue;
            }

            /* Self-loop verification: hex-dump the first returning frames. */
            if (iv->rxFrameLogged < 8) {
                uint32_t n = plen < 20 ? plen : 20;
                char line[3 * 20 + 1];
                for (uint32_t k = 0; k < n; k++) {
                    static const char hex[] = "0123456789abcdef";
                    line[k * 3 + 0] = hex[(b[k] >> 4) & 0xF];
                    line[k * 3 + 1] = hex[b[k] & 0xF];
                    line[k * 3 + 2] = ' ';
                }
                line[n * 3] = '\0';
                LOG("rx frame #%u len %u EtherType=0x%04x vaddr=%p io=0x%llx: %{public}s",
                    iv->rxFrameLogged, plen, et,
                    (void *)(uintptr_t)pkt->getDataVirtualAddress(),
                    (unsigned long long)pkt->getDataIOVirtualAddress(), line);
                iv->rxFrameLogged++;
            }
        }

        iv->rxFrameCount++;

        kern_return_t sret = pkt->setDataLength(plen);
        if (sret != kIOReturnSuccess) {
            iv->rxErrors++;
            rx_recycle_packet(iv, pkt);
        } else if (rx_deliver_packet(iv, pkt) != kIOReturnSuccess) {
            iv->rxErrors++;
            iv->rxDropped++;
            rx_recycle_packet(iv, pkt);
        } else {
            iv->rxFrames++;
        }

        d[3] = 0;
        iv->rxPending--;
        iv->rxClean = ring_next(slot, YT6801_RX_RING_DESC);
        harvested++;
    }

    if (harvested > 0) {
        iv->rxHarvested = harvested;
        iv->rxIdleChecks = 0;
        /* Re-arm immediately or the hardware soon runs out of buffers. */
        refillRx();
    } else if (iv->rxPending == 0) {
        /* Everything was harvested and nothing re-armed: wake the ring. */
        refillRx();
        if (++iv->rxIdleChecks >= 400) {
            iv->rxIdleChecks = 0;
            LOG("rx stalled: pending=0 free=%u CARDR=0x%08x RDTR=0x%08x SR=0x%08x",
                iv->rxFreeCount,
                reg_read(iv, dma_ch_reg(DMA_CH_CARDR_LO)),
                reg_read(iv, dma_ch_reg(DMA_CH_RDTR_LO)),
                reg_read(iv, dma_ch_reg(DMA_CH_SR)));
        }
    } else if (++iv->rxIdleChecks >= 400) {
        /* Buffers armed but the hardware wrote nothing for ~2s. Re-writing
         * the tail restarts a receive process stopped by RPS/RU. */
        iv->rxIdleChecks = 0;
        pushRxTail(iv);
        LOG("rx idle: pending=%u free=%u CARDR=0x%08x RDTR=0x%08x SR=0x%08x",
            iv->rxPending, iv->rxFreeCount,
            reg_read(iv, dma_ch_reg(DMA_CH_CARDR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_RDTR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_SR)));
    }
}

/* ==================================================================
 * MAC-side speed/duplex — the PS/FES/DM bits of MAC_CR
 *
 *   1000M: PS=0 FES=0   (GMII, GTX_CLK 125MHz)
 *   2500M: PS=0 FES=1   (fxgmii 2500, 312.5MHz)
 *    100M: PS=1 FES=1   (MII, TX_CLK 25MHz)
 *     10M: PS=1 FES=0   (MII, TX_CLK 2.5MHz)
 * ================================================================== */
static void
yt_set_mac_speed(YT * iv, uint32_t speed_mbps, bool full_duplex)
{
    uint32_t v = reg_read(iv, MAC_CR);

    v &= ~((1u << MAC_CR_PS_POS) | (1u << MAC_CR_FES_POS) | (1u << MAC_CR_DM_POS));
    switch (speed_mbps) {
    case 2500:
        v |= (1u << MAC_CR_FES_POS);                            /* PS=0 FES=1 */
        break;
    case 1000:
        break;                                                  /* PS=0 FES=0 */
    case 100:
        v |= (1u << MAC_CR_PS_POS) | (1u << MAC_CR_FES_POS);    /* PS=1 FES=1 */
        break;
    default:
        v |= (1u << MAC_CR_PS_POS);                             /* PS=1 FES=0 */
        break;
    }
    if (full_duplex) {
        v |= (1u << MAC_CR_DM_POS);
    }
    reg_write(iv, MAC_CR, v);
}

/* Configure the MAC-side speed from the current PHY link. */
static uint32_t
yt_sync_mac_speed(YT * iv, bool * fullOut)
{
    uint32_t spd = iv->linkSpeedMbps;
    bool full = iv->linkFullDuplex;

    if (spd == 0) {
        spd = 1000;
        full = true;
    }
    yt_set_mac_speed(iv, spd, full);
    if (fullOut != nullptr) {
        *fullOut = full;
    }
    return spd;
}

/* Extern linkage wrapper for yt6801.cpp link polling. */
uint32_t
yt_sync_mac_speed_shared(YT * iv)
{
    return yt_sync_mac_speed(iv, nullptr);
}

/* ==================================================================
 * TX ring
 * ================================================================== */

static inline uint32_t
tx_avail(YT * iv)
{
    /* Keep one slot free to distinguish full from empty. */
    return (iv->txClean - iv->txHead - 1) & (YT6801_TX_RING_DESC - 1);
}

/*
 * Write one frame into a descriptor and hand it to the hardware.
 *
 * A single-buffer frame is FD+LD on the same descriptor; the length
 * goes into desc2's B1L field (14 bits). CPC is fixed 0 (MAC adds CRC
 * and padding), IC=1 for a completion interrupt per frame.
 */
static kern_return_t
tx_submit_packet(YT * iv, IOUserNetworkPacket * pkt)
{
    if (tx_avail(iv) == 0) {
        return kIOReturnNoResources;
    }

    uint32_t len = pkt->getDataLength();

    /* Frame length cap = mtu + 14 (Ethernet header) + 4 (VLAN). */
    {
        uint32_t mtu = (iv->mtu != 0) ? iv->mtu : YT6801_DEFAULT_MTU;
        if (len == 0 || len > mtu + 18) {
            iv->txTooLong++;
            return kIOReturnBadArgument;
        }
    }

    /* TX data goes through driver-allocated buffers; copy from the pool packet. */
    uint64_t buf;
    uint32_t i = iv->txHead;

    if (iv->txSlotPhys[i] != 0 && iv->txSlotMap[i] != nullptr &&
        len <= YT6801_RX_BUFFER_SIZE) {
        uint8_t * dst = (uint8_t *)(uintptr_t)iv->txSlotMap[i]->GetAddress();
        const uint8_t * src = (const uint8_t *)(uintptr_t)pkt->getDataVirtualAddress();
        if (dst != nullptr && src != nullptr) {
            for (uint32_t k = 0; k < len; k++) {
                dst[k] = src[k];
            }
            /* The copy must be in memory before OWN is handed over. */
            OSSynchronizeIO();
            buf = iv->txSlotPhys[i];
        } else {
            buf = (uint64_t)pkt->getDataIOVirtualAddress();
        }
    } else {
        buf = (uint64_t)pkt->getDataIOVirtualAddress();
    }

    volatile uint32_t * d = desc_at(iv->txDesc, i);

    d[0] = (uint32_t)(buf & 0xFFFFFFFFull);
    /* TDES1 = buffer address high 32 bits (with EAME the hardware assembles the 64-bit address). */
    d[1] = (uint32_t)(buf >> 32);
    d[2] = (len << TDESC2_HL_B1L_POS) | (1u << TDESC2_IC_POS);
    /* FD | LD | FL, CTXT=0, CPC=0. FL (total frame length) is mandatory. */
    d[3] = (1u << TDESC3_FD_POS) | (1u << TDESC3_LD_POS)
         | ((len & TDESC3_FL_MASK) << TDESC3_FL_POS);

    OSSynchronizeIO();

    /* OWN goes last — once set, the hardware may read the descriptor immediately. */
    d[3] |= (1u << TDESC3_OWN_POS);
    OSSynchronizeIO();

    /* Packet returned immediately; slot reuse is decided by reclaimTx via CATDR. */
    iv->txPackets[i] = nullptr;
    iv->txHead = ring_next(i, YT6801_TX_RING_DESC);

    /* Tail pointer = device address of the next free descriptor. */
    {
        uint64_t tail = iv->txRingPhys + (uint64_t)iv->txHead * YT6801_DESC_SIZE;
        /* Same as RX: only TDTR_LO, high bits come from TDLR_HI. */
        reg_write(iv, dma_ch_reg(DMA_CH_TDTR_LO), (uint32_t)(tail & 0xFFFFFFFFull));
    }

    return kIOReturnSuccess;
}

void
io_miniyv_yt6801ethernet::serviceTx()
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady || !iv->interfaceEnabled) {
        return;
    }
    if (iv->txServicing) {
        return;                         /* already inside, let it loop */
    }
    if (iv->txSubmit == nullptr) {
        return;
    }

    iv->txServicing = true;
    iv->txServiceCount++;

    /* Take at most a full ring per call. */
    for (uint32_t n = 0; n < YT6801_TX_RING_DESC; n++) {
        if (tx_avail(iv) == 0) {
            /* Short bounded spin (~300us); the 1ms poll timer re-runs
             * serviceTx via reclaimTx if the ring is still full. */
            iv->txNoSpace++;
            bool freed = false;
            for (int k = 0; k < 60; k++) {
                reclaimTx();
                if (tx_avail(iv) > 0) {
                    freed = true;
                    break;
                }
                IODelay(5);
            }
            if (!freed) {
                break;
            }
        }

        IOUserNetworkPacket * pkt = nullptr;
        uint32_t got = iv->txSubmit->DequeuePackets(&pkt, 1);
        if (got == 0) {
            break;                      /* queue empty, normal exit */
        }
        if (pkt == nullptr) {
            iv->txFailed++;
            break;
        }

        kern_return_t ret = tx_submit_packet(iv, pkt);
        if (ret != kIOReturnSuccess) {
            iv->txFailed++;
            tx_return_packet(iv, pkt);
            if (ret == kIOReturnBadArgument) {
                /* Invalid packet: drop it and keep going. */
                continue;
            }
            break;                      /* resource issue (ring full), wait for next doorbell */
        }
        iv->txFrames++;
        if (iv->txFrames <= 6) {
            uint32_t len = pkt->getDataLength();
            const uint8_t * b = (const uint8_t *)(uintptr_t)pkt->getDataVirtualAddress();
            uint16_t et = 0;
            if (b != nullptr && len >= 14) {
                et = (uint16_t)((b[12] << 8) | b[13]);
            }
            LOG("tx frame #%u from stack: len %u EtherType=0x%04x", iv->txFrames, len, et);
        }
        /* Data already copied into the driver buffer; return the packet now. */
        tx_return_packet(iv, pkt);
    }

    iv->txServicing = false;
}

void
io_miniyv_yt6801ethernet::reclaimTx()
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady) {
        return;
    }

    uint32_t freed = 0;

    /* Two completion criteria, either is sufficient:
     * 1. descriptor OWN cleared (standard)
     * 2. CATDR pushed past it (OWN writeback is unreliable on this IP) */
    {
        uint32_t catdr = reg_read(iv, dma_ch_reg(DMA_CH_CATDR_LO));
        uint32_t base  = (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull);
        uint32_t idx   = ((catdr - base) / YT6801_DESC_SIZE) & (YT6801_TX_RING_DESC - 1);
        uint32_t ahead = (idx - iv->txClean) & (YT6801_TX_RING_DESC - 1);

        while (iv->txClean != iv->txHead) {
            volatile uint32_t * d = desc_at(iv->txDesc, iv->txClean);
            bool done = (d[3] & (1u << TDESC3_OWN_POS)) == 0;

            if (!done) {
                if (ahead <= 4) {
                    break;
                }
                ahead--;
            }

            IOUserNetworkPacket * pkt = iv->txPackets[iv->txClean];
            iv->txPackets[iv->txClean] = nullptr;
            tx_return_packet(iv, pkt);
            iv->txClean = ring_next(iv->txClean, YT6801_TX_RING_DESC);
            freed++;
        }
    }

    /* Space just freed; the doorbell only fires on the empty->non-empty edge. */
    if (freed > 0) {
        serviceTx();
    }
}

/* ==================================================================
 * Interrupts and doorbells
 * ================================================================== */
void
IMPL(io_miniyv_yt6801ethernet, InterruptOccurred)
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady) {
        return;
    }

    /* Clear status before working: this MSI covers both MAC and DMA,
     * and leftover state would re-trigger immediately. */
    uint32_t dma_isr = reg_read(iv, DMA_ISR);
    uint32_t ch_sr   = reg_read(iv, dma_ch_reg(DMA_CH_SR));
    uint32_t mac_isr = reg_read(iv, MAC_ISR);

    if (mac_isr != 0) {
        reg_write(iv, MAC_ISR, mac_isr);
    }
    if (dma_isr != 0) {
        reg_write(iv, DMA_ISR, dma_isr);
    }
    if (ch_sr != 0) {
        reg_write(iv, dma_ch_reg(DMA_CH_SR), ch_sr);
    }

    /* RX: completed frames go to the stack. */
    if (ch_sr & (1u << DMA_CH_SR_RI_POS)) {
        serviceRx();
    }
    /* TX: reclaimed descriptors go back to the stack. */
    if (ch_sr & (1u << DMA_CH_SR_TI_POS)) {
        reclaimTx();
    }
    /* RX buffer unavailable: pool temporarily empty, refill later. */
    if (ch_sr & (1u << DMA_CH_SR_RBU_POS)) {
        iv->rxNoPacket++;
        refillRx();
    }
    /* PHY status change (cable plug/unplug): the sole link-state driver. */
    if (mac_isr & (1u << MAC_ISR_PHYIS_POS)) {
        pollLinkStatus();
    }
}

void
IMPL(io_miniyv_yt6801ethernet, TxDataAvailable)
{
    if (ivars == nullptr) {
        return;
    }
    ivars->doorbellCount++;
    /* Reclaim finished descriptors before sending new ones, or the 128
     * ring fills up and tx_avail stays 0. */
    reclaimTx();
    serviceTx();

    /* Harvest RX here as well: MSI-X may not be delivered, but the TX
     * doorbell fires per packet, so RX never stalls permanently. */
    serviceRx();
}

void
IMPL(io_miniyv_yt6801ethernet, RxDataAvailable)
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady || iv->rxSubmit == nullptr) {
        return;
    }

    /* The stack returned used packets; stage them and re-arm the ring. */
    for (uint32_t n = 0; n < YT6801_RX_RING_DESC; n++) {
        if (iv->rxFreeCount >= YT6801_RX_RING_DESC) {
            break;
        }
        IOUserNetworkPacket * pkt = nullptr;
        uint32_t got = iv->rxSubmit->DequeuePackets(&pkt, 1);
        if (got == 0 || pkt == nullptr) {
            break;
        }
        iv->rxFree[iv->rxFreeCount++] = pkt;
    }

    refillRx();

    /* A packet return means another RX round can run. */
    serviceRx();
}

/* ==================================================================
 * Real-machine loopback self-test (no cable needed)
 *
 * Builds an Ethernet frame and walks the full path:
 *   TX desc -> DMA read -> MAC TX -> PHY internal loopback
 *     -> MAC RX -> DMA write -> RX desc -> byte-for-byte compare
 *
 * Compares the bytes we wrote, not the hardware's claim of success;
 * a result bitmap of 0 is required to pass. Assumes setupDatapath
 * already configured the rings and MTL and the PHY is in loopback.
 * DMA/MAC are temporarily opened here and shut down afterwards.
 * ================================================================== */
#define YT_LB_FRAME_LEN   128
#define YT_LB_ETHERTYPE   0x88B5u   /* experimental EtherType, ignored by the stack */

kern_return_t
io_miniyv_yt6801ethernet::runHardwareLoopbackTest(uint64_t * result)
{
    YT * iv = ivars;
    IOUserNetworkPacket * rxPkt = nullptr;
    IOUserNetworkPacket * txPkt = nullptr;
    uint64_t bits = 0;
    uint32_t savedMacCr = 0;
    uint32_t savedPfr = 0;
    uint32_t savedEcr = 0;
    uint32_t mmcTxBefore = 0;
    uint32_t mmcUnderBefore = 0;
    uint32_t mmcRxBefore = 0;
    uint32_t mmcCrcBefore = 0;
    bool dmaStarted = false;
    kern_return_t ret = kIOReturnError;

    /* Loopback-test private DMA buffers: self-mapped, address is valid. */
    IOBufferMemoryDescriptor * lbTxMem = nullptr;
    IOMemoryMap *              lbTxMap = nullptr;
    IODMACommand *             lbTxDma = nullptr;
    volatile uint32_t *        lbTxDesc = nullptr;
    uint64_t                   lbTxPhys = 0;

    IOBufferMemoryDescriptor * lbRxMem = nullptr;
    IOMemoryMap *              lbRxMap = nullptr;
    IODMACommand *             lbRxDma = nullptr;
    volatile uint32_t *        lbRxDesc = nullptr;
    uint64_t                   lbRxPhys = 0;


    if (result) {
        *result = ~0ull;
    }
    if (iv == nullptr || iv->pool == nullptr ||
        iv->txDesc == nullptr || iv->rxDesc == nullptr) {
        LOG("hw loopback: datapath not ready, skipping");
        return kIOReturnNotReady;
    }

    if (iv->pool->allocatePacket(&rxPkt) != kIOReturnSuccess || rxPkt == nullptr ||
        iv->pool->allocatePacket(&txPkt) != kIOReturnSuccess || txPkt == nullptr) {
        LOG("hw loopback: no test packets from pool");
        bits |= (1ull << 8);
        goto out;
    }

    /* ---- 1. Build the frame: dst/src are our own MAC, payload is a
     * recomputable incrementing pattern. ---- */
    {
        uint8_t * f = (uint8_t *)(uintptr_t)txPkt->getDataVirtualAddress();
        for (int i = 0; i < YT6801_MAC_LEN; i++) {
            f[i]     = iv->macAddress[i];
            f[6 + i] = iv->macAddress[i];
        }
        f[12] = (uint8_t)(YT_LB_ETHERTYPE >> 8);
        f[13] = (uint8_t)(YT_LB_ETHERTYPE & 0xFF);
        for (uint32_t i = 14; i < YT_LB_FRAME_LEN; i++) {
            f[i] = (uint8_t)(0xA5u ^ (i * 7u));      /* fixed pattern */
        }
        txPkt->setDataLength(YT_LB_FRAME_LEN);
    }

    /* ---- 2. Arm only descriptor 0 so the returning frame must land there.
     *
     * Pre-fill the RX buffer with 0xAA: the only reliable verdict on
     * whether the hardware wrote this memory at all.
     *   still 0xAA after loopback -> hardware never wrote (bad address)
     *   0x00 after loopback       -> wrote an empty frame
     *   correct content           -> path works ---- */
    {
        uint8_t * r = (uint8_t *)(uintptr_t)rxPkt->getDataVirtualAddress();
        for (uint32_t i = 0; i < YT6801_RX_BUFFER_SIZE; i++) {
            r[i] = 0xAA;
        }
        /* Fill offset 1024 of the TX packet buffer with 0x55: the RX
         * descriptor temporarily points here to test that IOVA alone. */
        {
            uint8_t * t = (uint8_t *)(uintptr_t)txPkt->getDataVirtualAddress();
            for (uint32_t i = 1024; i < 1024 + 256; i++) {
                t[i] = 0x55;
            }
        }
    }
    rx_arm_descriptor(iv, 0, rxPkt);

    /* ---- 2b. The loopback test uses driver-allocated DMA buffers ----
     * Pool packets' getDataIOVirtualAddress() is not writable by the
     * hardware; the self-mapped buffers are guaranteed valid. This way
     * the test measures the hardware path itself (descriptor format,
     * DMA, MTL, MAC, loopback), independent of the pool mapping. */
    {
        ret = alloc_dma_ring(this, iv, 4096,
                             &lbTxMem, &lbTxMap, &lbTxDma, &lbTxDesc, &lbTxPhys);
        kern_return_t rret = alloc_dma_ring(this, iv, 4096,
                             &lbRxMem, &lbRxMap, &lbRxDma, &lbRxDesc, &lbRxPhys);
        if (ret == kIOReturnSuccess && rret == kIOReturnSuccess &&
            lbTxDesc != nullptr && lbRxDesc != nullptr) {

            uint8_t * f = (uint8_t *)lbTxDesc;
            for (int i = 0; i < YT6801_MAC_LEN; i++) {
                f[i]     = iv->macAddress[i];
                f[6 + i] = iv->macAddress[i];
            }
            f[12] = (uint8_t)(YT_LB_ETHERTYPE >> 8);
            f[13] = (uint8_t)(YT_LB_ETHERTYPE & 0xFF);
            for (uint32_t i = 14; i < YT_LB_FRAME_LEN; i++) {
                f[i] = (uint8_t)(0xA5u ^ (i * 7u));
            }

            uint8_t * r = (uint8_t *)lbRxDesc;
            for (uint32_t i = 0; i < 4096; i++) {
                r[i] = 0xAA;
            }
            OSSynchronizeIO();

            /* Point the RX descriptor at the driver-allocated buffer. */
            {
                volatile uint32_t * d = desc_at(iv->rxDesc, 0);
                d[0] = (uint32_t)(lbRxPhys & 0xFFFFFFFFull);
                d[1] = (uint32_t)(lbRxPhys >> 32);
                d[2] = (uint32_t)(YT6801_RX_BUFFER_SIZE & 0x3FFFu);
                d[3] = (1u << RDESC3_INTE_POS) | (1u << RDESC3_BUF1V_POS)
                     | (1u << RDESC3_OWN_POS);
                OSSynchronizeIO();
            }

            LOG("loopback buffers ready TX=0x%llx RX=0x%llx (first 6 bytes %02x %02x %02x %02x %02x %02x)",
                (unsigned long long)lbTxPhys, (unsigned long long)lbRxPhys,
                f[0], f[1], f[2], f[3], f[4], f[5]);
        } else {
            LOG("loopback buffer alloc failed tx=0x%x rx=0x%x", ret, rret);
        }
        ret = kIOReturnError;
    }

    /* ---- 3. Fill TX descriptor 0 but do not hand over OWN yet. ---- */
    {
        volatile uint32_t * d = desc_at(iv->txDesc, 0);
        uint64_t buf = lbTxPhys ? lbTxPhys
                                : (uint64_t)txPkt->getDataIOVirtualAddress();

        d[0] = (uint32_t)(buf & 0xFFFFFFFFull);
        d[1] = (uint32_t)(buf >> 32);               /* address high 32 bits, mandatory */
        d[2] = (YT_LB_FRAME_LEN << TDESC2_HL_B1L_POS) | (1u << TDESC2_IC_POS);
        /* FD | LD | FL — FL is mandatory, see tx_submit_packet */
        d[3] = (1u << TDESC3_FD_POS) | (1u << TDESC3_LD_POS)
             | ((YT_LB_FRAME_LEN & TDESC3_FL_MASK) << TDESC3_FL_POS);
        OSSynchronizeIO();
    }

    /* ---- 4. Temporarily open the DMA and MAC gates (the real switch is
     * enableDatapath). Order matters: TX queue, DMA channel and MAC
     * gates all on, before setting OWN and pushing the tail pointer. ---- */
    savedMacCr = reg_read(iv, MAC_CR);

    /* Probe whether the ring base register is writable: write a magic,
     * read back, then write the real value. */
    {
        reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), 0xDEADBEEFu);
        uint32_t probe = reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO));
        LOG("TDLR magic probe readback=0x%08x (0xdeadbeef=writable, 0=not writable)", probe);

        /* Rewrite the real values regardless; order: length then base. */
        reg_write(iv, dma_ch_reg(DMA_CH_TDRLR), YT6801_TX_RING_DESC - 1);
        reg_write(iv, dma_ch_reg(DMA_CH_TDLR_HI), (uint32_t)(iv->txRingPhys >> 32));
        reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull));
        reg_write(iv, dma_ch_reg(DMA_CH_RDRLR), YT6801_RX_RING_DESC - 1);
        reg_write(iv, dma_ch_reg(DMA_CH_RDLR_HI), (uint32_t)(iv->rxRingPhys >> 32));
        reg_write(iv, dma_ch_reg(DMA_CH_RDLR_LO), (uint32_t)(iv->rxRingPhys & 0xFFFFFFFFull));
        LOG("rewritten TDLR=0x%08x TDRLR=%u RDLR=0x%08x RDRLR=%u",
            reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)),
            reg_read(iv, dma_ch_reg(DMA_CH_RDLR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_RDRLR)));
    }

    reg_write(iv, dma_ch_reg(DMA_CH_RCR),
              (reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~0x3u)
              | (1u << DMA_CH_RCR_SR_POS)
              | ((YT6801_RX_BUFFER_SIZE << DMA_CH_RCR_RBSZ_POS)
                 & (0x3FFFu << DMA_CH_RCR_RBSZ_POS)));
    /* Write 1 to clear all channel status bits: leftover TI/TBU/FBE from
     * a previous round would leave the channel half-open. */
    reg_write(iv, dma_ch_reg(DMA_CH_SR), 0xFFFFFFFFu);

    /* ST=1 start, OSF=1: let the DMA wait for the whole frame in the
     * FIFO before notifying the MAC, avoiding partial frames. */
    reg_write(iv, dma_ch_reg(DMA_CH_TCR),
              reg_read(iv, dma_ch_reg(DMA_CH_TCR))
              | (1u << DMA_CH_TCR_ST_POS) | (1u << DMA_CH_TCR_OSF_POS));
    /* MAC-side speed/duplex follows the link: self-loop is 1000M full
     * duplex GMII (PS=0/FES=0/DM=1), matching the PHY's 0x4140. */
    {
        bool fd = true;
        uint32_t spd = yt_sync_mac_speed(iv, &fd);
        LOG("MAC speed from link: %uM %s MAC_CR=0x%08x",
            spd, fd ? "full duplex" : "half duplex", reg_read(iv, MAC_CR));
    }
    reg_write(iv, MAC_CR,
              reg_read(iv, MAC_CR)
              | (1u << MAC_CR_RE_POS) | (1u << MAC_CR_TE_POS)
              | (1u << MAC_CR_CST_POS) | (1u << MAC_CR_ACS_POS));

    /* Open every receive-side gate to narrow the problem:
     *   PFR.RA=1  receive all (bypass DA filtering)
     *   PFR.PR=1  promiscuous
     *   ECR.DCRCC=1 disable RX CRC check (loopback FCS may be wrong)
     * If frames still do not arrive, filtering/CRC is not the cause. */
    {
        uint32_t pfr = reg_read(iv, MAC_PFR);
        savedPfr = pfr;
        savedEcr = reg_read(iv, MAC_ECR);
        reg_write(iv, MAC_PFR, pfr | (1u << MAC_PFR_RA_POS) | (1u << MAC_PFR_PR_POS));
        reg_write(iv, MAC_ECR, savedEcr | (1u << MAC_ECR_DCRCC_POS));
        LOG("loopback filters relaxed: PFR 0x%08x->0x%08x ECR=0x%08x PHYIF_STA=0x%08x",
            pfr, reg_read(iv, MAC_PFR), reg_read(iv, MAC_ECR),
            reg_read(iv, MAC_PHYIF_STA));
    }
    LOG("loopback mode: GMII 1000M full duplex MAC_CR=0x%08x (PS/FES/DM=0/0/1)",
        reg_read(iv, MAC_CR));
    dmaStarted = true;

    /* Rewrite the ring base/length after the channel started: those
     * registers may only accept configuration while the channel runs. */
    reg_write(iv, dma_ch_reg(DMA_CH_TDRLR), YT6801_TX_RING_DESC - 1);
    reg_write(iv, dma_ch_reg(DMA_CH_TDLR_HI), (uint32_t)(iv->txRingPhys >> 32));
    reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull));
    reg_write(iv, dma_ch_reg(DMA_CH_RDRLR), YT6801_RX_RING_DESC - 1);
    reg_write(iv, dma_ch_reg(DMA_CH_RDLR_HI), (uint32_t)(iv->rxRingPhys >> 32));
    reg_write(iv, dma_ch_reg(DMA_CH_RDLR_LO), (uint32_t)(iv->rxRingPhys & 0xFFFFFFFFull));
    LOG("rewritten after channel start: TDLR=0x%08x TDRLR=%u TCR=0x%08x",
        reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
        reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)),
        reg_read(iv, dma_ch_reg(DMA_CH_TCR)));

    /* RX descriptor 0 is armed; push the tail pointer to slot 1. */
    reg_write(iv, dma_ch_reg(DMA_CH_RDTR_LO),
              (uint32_t)((iv->rxRingPhys + (uint64_t)YT6801_DESC_SIZE) & 0xFFFFFFFFull));

    /* ---- 4b. Register readbacks: separate "write didn't stick" from
     * "DMA not running". A TX descriptor not being taken has three
     * possible causes: value didn't stick, channel stopped (TPS), or a
     * bus error (FBE). ---- */
    LOG("self-test regs TDLR=0x%08x TDRLR=%u TCR=0x%08x RCR=0x%08x",
        reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
        reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)),
        reg_read(iv, dma_ch_reg(DMA_CH_TCR)),
        reg_read(iv, dma_ch_reg(DMA_CH_RCR)));
    LOG("self-test regs TQOMR=0x%08x RQOMR=0x%08x SBMR=0x%08x MR=0x%08x MACCR=0x%08x",
        reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)),
        reg_read(iv, mtl_q_reg(0, MTL_Q_RQOMR)),
        reg_read(iv, DMA_SBMR),
        reg_read(iv, DMA_MR),
        reg_read(iv, MAC_CR));
    LOG("MTL queue underflow=0x%08x debug=0x%08x (FIFO content / MAC draining)",
        reg_read(iv, mtl_q_reg(0, 0x04)),
        reg_read(iv, mtl_q_reg(0, 0x08)));
    LOG("self-test regs CH_SR=0x%08x CATDR=0x%08x CATBR=0x%08x DESC=0x%llx DBGSTA=0x%08x",
        reg_read(iv, dma_ch_reg(DMA_CH_SR)),
        reg_read(iv, dma_ch_reg(DMA_CH_CATDR_LO)),
        reg_read(iv, dma_ch_reg(DMA_CH_CATBR_LO)),
        (unsigned long long)iv->txRingPhys,
        reg_read(iv, MAC_DBG_STA));

    /* ---- 5. Two-phase loopback experiment ----
     * Phase 0: MAC internal loopback (MAC_CR.LM=1), frame folds back at
     *          the GMII interface without the PHY.
     * Phase 1: only the PHY digital loopback (LM=0, ext 0x0A=0x3A18).
     * Each phase uses its own descriptor slot; sharing one slot would
     * trip the head==tail empty condition. */
    {
        bool got = false;
        int  gotPhase = -1;
        uint32_t d3 = 0;

        for (int phase = 0; phase < 2 && !got; phase++) {
            const char * name = (phase == 0) ? "MAC internal loopback" : "PHY digital loopback";

            /* Switch the loopback path. */
            {
                uint32_t cr = reg_read(iv, MAC_CR);
                if (phase == 0) {
                    cr |= (1u << MAC_CR_LM_POS);
                } else {
                    cr &= ~(1u << MAC_CR_LM_POS);
                }
                reg_write(iv, MAC_CR, cr);
                LOG("%s: MAC_CR wrote 0x%08x readback 0x%08x (readback 0 = MAC internal loopback not implemented)",
                    name, cr, reg_read(iv, MAC_CR));
            }

            /* Re-arm this RX slot (idempotent). */
            rx_arm_descriptor(iv, (uint32_t)phase, rxPkt);
            reg_write(iv, dma_ch_reg(DMA_CH_RDTR_LO),
                      (uint32_t)((iv->rxRingPhys
                                  + (uint64_t)(phase + 1) * YT6801_DESC_SIZE)
                                 & 0xFFFFFFFFull));

            /* Fill this TX descriptor. */
            {
                volatile uint32_t * d = desc_at(iv->txDesc, (uint32_t)phase);
                uint64_t buf = lbTxPhys ? lbTxPhys
                                : (uint64_t)txPkt->getDataIOVirtualAddress();

                mmcTxBefore    = reg_read(iv, 0x0718);   /* MMC_TXFRAMECOUNT_GB_LO */
                mmcUnderBefore = reg_read(iv, 0x0748);   /* MMC_TXUNDERFLOWERROR_LO */
                mmcRxBefore    = reg_read(iv, 0x0780);   /* MMC_RXFRAMECOUNT_GB_LO */
                mmcCrcBefore   = reg_read(iv, 0x0794);   /* MMC_RXCRCERROR_LO */

                d[0] = (uint32_t)(buf & 0xFFFFFFFFull);
                d[1] = (uint32_t)(buf >> 32);
                d[2] = (YT_LB_FRAME_LEN << TDESC2_HL_B1L_POS) | (1u << TDESC2_IC_POS);
                d[3] = (1u << TDESC3_FD_POS) | (1u << TDESC3_LD_POS)
                     | ((YT_LB_FRAME_LEN & TDESC3_FL_MASK) << TDESC3_FL_POS);
                OSSynchronizeIO();
                LOG("%s: TX desc d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x (pkt IOVA=0x%llx len %u)",
                    name, d[0], d[1], d[2], d[3],
                    (unsigned long long)buf, txPkt->getDataLength());

                d[3] |= (1u << TDESC3_OWN_POS);
                OSSynchronizeIO();
                reg_write(iv, dma_ch_reg(DMA_CH_TDTR_LO),
                          (uint32_t)((iv->txRingPhys
                                      + (uint64_t)(phase + 1) * YT6801_DESC_SIZE)
                                     & 0xFFFFFFFFull));
            }

            /* Wait up to 150ms for this RX descriptor to be written back. */
            for (int i = 0; i < 1500; i++) {
                d3 = desc_at(iv->rxDesc, (uint32_t)phase)[3];
                if ((d3 & (1u << RDESC3_OWN_POS)) == 0) {
                    got = true;
                    gotPhase = phase;
                    break;
                }
                IODelay(100);
            }

            {
                uint32_t txAfter    = reg_read(iv, 0x0718);
                uint32_t underAfter = reg_read(iv, 0x0748);
                uint32_t rxAfter    = reg_read(iv, 0x0780);
                uint32_t crcAfter   = reg_read(iv, 0x0794);
                LOG("%s: %s | MMC tx +%u underflow +%u rx +%u crcErr +%u",
                    name, got ? "received" : "not received",
                    txAfter - mmcTxBefore, underAfter - mmcUnderBefore,
                    rxAfter - mmcRxBefore, crcAfter - mmcCrcBefore);
                LOG("  side: RXQ debug(0x0C)=0x%08x PHYIF_STA=0x%08x ISR=0x%08x"
                    " alignErr=0x%08x oversize=0x%08x runt=0x%08x",
                    reg_read(iv, mtl_q_reg(0, 0x0C)),
                    reg_read(iv, MAC_PHYIF_STA),
                    reg_read(iv, MAC_ISR),
                    reg_read(iv, 0x0798),      /* MMC_RXALIGNERROR */
                    reg_read(iv, 0x0790),      /* MMC_RXOVERSIZEFRAMES */
                    reg_read(iv, 0x07A0));     /* MMC_RXRUNTERROR */
            }
        }

        if (!got) {
            bits |= (1ull << 1);
            LOG("hw loopback: no frame returned on either path");
            LOG("after failure MTL underflow=0x%08x debug=0x%08x",
                reg_read(iv, mtl_q_reg(0, 0x04)),
                reg_read(iv, mtl_q_reg(0, 0x08)));
            LOG("after failure CH_SR=0x%08x CATDR=0x%08x CATBR=0x%08x TDTR=0x%08x",
                reg_read(iv, dma_ch_reg(DMA_CH_SR)),
                reg_read(iv, dma_ch_reg(DMA_CH_CATDR_LO)),
                reg_read(iv, dma_ch_reg(DMA_CH_CATBR_LO)),
                reg_read(iv, dma_ch_reg(DMA_CH_TDTR_LO)));
            LOG("after failure MAC_CR=0x%08x PFR=0x%08x ECR=0x%08x ISR=0x%08x",
                reg_read(iv, MAC_CR),
                reg_read(iv, MAC_PFR),
                reg_read(iv, MAC_ECR),
                reg_read(iv, MAC_ISR));
        } else {
            uint32_t plen = d3 & RDESC3_PL_MASK;
            bool es = (d3 & (1u << RDESC3_ES_POS)) != 0;

            iv->hwLoopbackTxLen = YT_LB_FRAME_LEN;
            iv->hwLoopbackRxLen = plen;
            LOG("loopback path hit: %s (PL=%u)", gotPhase == 0 ? "MAC internal loopback" : "PHY digital loopback", plen);

            if (es) {
                bits |= (1ull << 4);
                LOG("hw loopback: hardware set error bit ES, PL=%u", plen);
            }

            /* Whether PL includes the 4-byte CRC is configuration
             * dependent; accept both and compare the smaller length. */
            uint32_t cmp = plen;
            if (cmp > YT_LB_FRAME_LEN) {
                cmp = YT_LB_FRAME_LEN;
            }
            if (plen < YT_LB_FRAME_LEN) {
                bits |= (1ull << 2);
                LOG("hw loopback: received frame shorter than sent PL=%u expected>=%u",
                    plen, (unsigned)YT_LB_FRAME_LEN);
            }

            if (cmp > 0) {
                const uint8_t * r = lbRxDesc ? (const uint8_t *)(uintptr_t)lbRxDesc
                                             : (const uint8_t *)(uintptr_t)rxPkt->getDataVirtualAddress();
                const uint8_t * t = lbTxDesc ? (const uint8_t *)(uintptr_t)lbTxDesc
                                             : (const uint8_t *)(uintptr_t)txPkt->getDataVirtualAddress();

                /* CPU view vs device view: a nonzero delta means the
                 * descriptor address and the memcpy address differ. */
                LOG("addr check TX v=%p io=0x%llx | RX v=%p io=0x%llx",
                    t, (unsigned long long)txPkt->getDataIOVirtualAddress(),
                    r, (unsigned long long)rxPkt->getDataIOVirtualAddress());
                LOG("RX desc writeback d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x",
                    desc_at(iv->rxDesc, 0)[0], desc_at(iv->rxDesc, 0)[1],
                    desc_at(iv->rxDesc, 0)[2], desc_at(iv->rxDesc, 0)[3]);
                LOG("RX first 16 bytes %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x | "
                    "TX first 16 bytes %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x",
                    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                    r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15],
                    t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                    t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);

                uint32_t diff = 0;
                for (uint32_t i = 0; i < cmp; i++) {
                    if (r[i] != t[i]) {
                        if (diff == 0) {
                            LOG("hw loopback: byte %u mismatch sent 0x%02x got 0x%02x",
                                i, t[i], r[i]);
                        }
                        diff++;
                    }
                }
                if (diff != 0) {
                    bits |= (1ull << 3);
                    LOG("hw loopback: %u bytes mismatch (compared %u)", diff, cmp);
                }
            }
        }
    }

    /* ---- 6. Was the TX side consumed by hardware (OWN cleared)? ---- */
    if (desc_at(iv->txDesc, 0)[3] & (1u << TDESC3_OWN_POS)) {
        bits |= (1ull << 0);
        LOG("hw loopback: TX descriptor still owned by hardware after 200ms, DMA did not take it");
    }

out:
    if (lbTxDma != nullptr) {
        lbTxDma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    OSSafeReleaseNULL(lbTxDma);
    OSSafeReleaseNULL(lbTxMap);
    OSSafeReleaseNULL(lbTxMem);
    if (lbRxDma != nullptr) {
        lbRxDma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    OSSafeReleaseNULL(lbRxDma);
    OSSafeReleaseNULL(lbRxMap);
    OSSafeReleaseNULL(lbRxMem);
    /* ---- 7. Cleanup: stop DMA, clear descriptors, return packets. ---- */

    if (dmaStarted) {
        /* Restore the pre-test MAC_CR instead of clearing three bits:
         * the test may have reprogrammed PS/FES/DM to 100M MII. */
        reg_write(iv, MAC_CR, savedMacCr);
        /* Restore filtering/CRC checking. */
        reg_write(iv, MAC_PFR, savedPfr);
        reg_write(iv, MAC_ECR, savedEcr);
        reg_write(iv, dma_ch_reg(DMA_CH_TCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_TCR)) & ~(1u << DMA_CH_TCR_ST_POS));
        reg_write(iv, dma_ch_reg(DMA_CH_RCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~(1u << DMA_CH_RCR_SR_POS));
        IODelay(1000);      /* give the DMA time to finish in-flight writes */
    }
    if (iv->txDesc != nullptr) {
        for (int w = 0; w < YT6801_DESC_WORDS; w++) {
            desc_at(iv->txDesc, 0)[w] = 0;
        }
    }
    if (iv->rxDesc != nullptr) {
        for (int w = 0; w < YT6801_DESC_WORDS; w++) {
            desc_at(iv->rxDesc, 0)[w] = 0;
        }
    }
    OSSynchronizeIO();

    iv->rxPackets[0] = nullptr;
    iv->txPackets[0] = nullptr;
    iv->txHead = 0;
    iv->txClean = 0;
    iv->rxArm = 0;
    iv->rxClean = 0;
    iv->rxPending = 0;

    if (rxPkt != nullptr) {
        iv->pool->deallocatePacket(rxPkt);
    }
    if (txPkt != nullptr) {
        iv->pool->deallocatePacket(txPkt);
    }

    iv->hwLoopbackResult = bits;
    if (result) {
        *result = bits;
    }

    LOG("hw loopback test %s (sent %u bytes, received %u bytes, bitmap 0x%llx)",
        bits == 0 ? "passed" : "failed",
        iv->hwLoopbackTxLen, iv->hwLoopbackRxLen,
        (unsigned long long)bits);

    return bits == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

/* ==================================================================
 * Start/stop
 * ================================================================== */
void
io_miniyv_yt6801ethernet::enableDatapath(bool enable)
{
    YT * iv = ivars;

    if (iv == nullptr || !iv->datapathReady) {
        return;
    }

    if (enable) {
        /* ============================================================
         * Order is decisive, do not swap:
         *   stop channel -> clear status -> ring length/base -> start
         *   -> push tail pointer
         * Starting with ST=1 before TDLR is written makes the DMA fetch
         * descriptors from a zeroed base address and hang the channel.
         * ============================================================ */
        reg_write(iv, dma_ch_reg(DMA_CH_TCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_TCR)) & ~(1u << DMA_CH_TCR_ST_POS));
        reg_write(iv, dma_ch_reg(DMA_CH_RCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~(1u << DMA_CH_RCR_SR_POS));
        IODelay(1000);
        /* Clear leftover TI/TBU/FBE or the channel starts with stale state. */
        reg_write(iv, dma_ch_reg(DMA_CH_SR), 0xFFFFFFFFu);

        /* Write ring length and base while stopped; length first, then base. */
        reg_write(iv, dma_ch_reg(DMA_CH_TDRLR), YT6801_TX_RING_DESC - 1);
        reg_write(iv, dma_ch_reg(DMA_CH_TDLR_HI), (uint32_t)(iv->txRingPhys >> 32));
        reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull));
        reg_write(iv, dma_ch_reg(DMA_CH_RDRLR), YT6801_RX_RING_DESC - 1);
        reg_write(iv, dma_ch_reg(DMA_CH_RDLR_HI), (uint32_t)(iv->rxRingPhys >> 32));
        reg_write(iv, dma_ch_reg(DMA_CH_RDLR_LO), (uint32_t)(iv->rxRingPhys & 0xFFFFFFFFull));
        /* Empty ring: push the TX tail to the next-to-send slot (reuse
         * txHead) — do not hardcode the ring start, there may already be
         * queued descriptors. */
        reg_write(iv, dma_ch_reg(DMA_CH_TDTR_LO),
                  (uint32_t)((iv->txRingPhys + (uint64_t)iv->txHead * YT6801_DESC_SIZE)
                             & 0xFFFFFFFFull));

        LOG("ring base (written while stopped): TDLR=0x%08x RDLR=0x%08x TDRLR=%u (readback 0 while stopped is normal)",
            reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_RDLR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)));

        /* Start only after the base is settled. RX first, TX last. */
        reg_write(iv, dma_ch_reg(DMA_CH_RCR),
                  (reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~0x3u)
                  | (1u << DMA_CH_RCR_SR_POS)
                  | ((YT6801_RX_BUFFER_SIZE << DMA_CH_RCR_RBSZ_POS)
                     & (0x3FFFu << DMA_CH_RCR_RBSZ_POS)));
        reg_write(iv, dma_ch_reg(DMA_CH_TCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_TCR))
                  | (1u << DMA_CH_TCR_ST_POS) | (1u << DMA_CH_TCR_OSF_POS));
        IODelay(1000);

        /* Starting RX resets CARDR and RDTR to the ring base, and
         * CARDR == RDTR means "ring empty": the tail must be pushed
         * again, pointing at the last armed descriptor. */
        pushRxTail(iv);

        LOG("channel started: TDLR=0x%08x TDRLR=%u TCR=0x%08x RCR=0x%08x SR=0x%08x "
            "RDTR=0x%08x (rxArm=%u)",
            reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
            reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)),
            reg_read(iv, dma_ch_reg(DMA_CH_TCR)),
            reg_read(iv, dma_ch_reg(DMA_CH_RCR)),
            reg_read(iv, dma_ch_reg(DMA_CH_SR)),
            reg_read(iv, dma_ch_reg(DMA_CH_RDTR_LO)),
            iv->rxArm);

        /* Fallback: if the IP only accepts base writes while running
         * (stopped-state writes dropped, readback 0), rewrite once. */
        if (reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)) !=
            (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull)) {
            reg_write(iv, dma_ch_reg(DMA_CH_TDLR_HI), (uint32_t)(iv->txRingPhys >> 32));
            reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull));
            reg_write(iv, dma_ch_reg(DMA_CH_RDLR_HI), (uint32_t)(iv->rxRingPhys >> 32));
            reg_write(iv, dma_ch_reg(DMA_CH_RDLR_LO), (uint32_t)(iv->rxRingPhys & 0xFFFFFFFFull));
            LOG("stopped-state base dropped, rewritten: TDLR=0x%08x RDLR=0x%08x",
                reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
                reg_read(iv, dma_ch_reg(DMA_CH_RDLR_LO)));
        }

        reg_write(iv, dma_ch_reg(DMA_CH_IER),
                  (1u << DMA_CH_IER_TIE_POS) | (1u << DMA_CH_IER_RIE_POS)
                  | (1u << DMA_CH_IER_RBUE_POS) | (1u << DMA_CH_IER_FBEE_POS)
                  | (1u << DMA_CH_IER_AIE_POS) | (1u << DMA_CH_IER_NIE_POS));

        reg_write(iv, MAC_CR, reg_read(iv, MAC_CR)
                  | (1u << MAC_CR_RE_POS) | (1u << MAC_CR_TE_POS));

        /* Only the PHY status change interrupt is needed; the other MAC
         * interrupts (timestamp, LPI, PMT) are unused. */
        reg_write(iv, MAC_IER, reg_read(iv, MAC_IER) | (1u << MAC_ISR_PHYIS_POS));

        /* One extra doorbell: descriptors may already be queued from
         * before the interface opened, and the hardware does not look
         * back at the old ring on its own. */
        reg_write(iv, dma_ch_reg(DMA_CH_TDTR_LO),
                  (uint32_t)((iv->txRingPhys
                              + (uint64_t)iv->txHead * YT6801_DESC_SIZE) & 0xFFFFFFFFull));
        reg_write(iv, dma_ch_reg(DMA_CH_RDTR_LO),
                  (uint32_t)((iv->rxRingPhys
                              + (uint64_t)iv->rxArm * YT6801_DESC_SIZE) & 0xFFFFFFFFull));

        LOG("datapath started");
    } else {
        /* Disable in reverse: MAC first, then DMA. */
        reg_write(iv, MAC_CR, reg_read(iv, MAC_CR)
                  & ~((1u << MAC_CR_RE_POS) | (1u << MAC_CR_TE_POS)
                      | (1u << MAC_CR_LM_POS)));
        reg_write(iv, dma_ch_reg(DMA_CH_IER), 0);
        reg_write(iv, dma_ch_reg(DMA_CH_RCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~(1u << DMA_CH_RCR_SR_POS));
        reg_write(iv, dma_ch_reg(DMA_CH_TCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_TCR)) & ~(1u << DMA_CH_TCR_ST_POS));
        LOG("datapath stopped");
    }
}

/* ==================================================================
 * Init
 * ================================================================== */

/*
 * Per-queue FIFO size from the MAC_HWF1R reported capacity (256-byte
 * units, minus-one encoding):
 *   q_fifo = min(81920, 1 << (hw_feat + 7)) / queues / 256, minus one.
 */
static uint32_t
calc_per_queue_fifo(uint32_t hwFeatLog2, uint32_t queues)
{
    uint64_t q = 1ull << (hwFeatLog2 + 7);

    if (q > FXGMAC_MAX_FIFO) {
        q = FXGMAC_MAX_FIFO;
    }
    if (queues == 0) {
        queues = 1;
    }
    q /= queues;

    uint32_t p = (uint32_t)(q / 256);
    if (p != 0) {
        p--;
    }
    return p;
}

/*
 * DMA system bus mode (DMA_SBMR): enhanced addressing, read/write
 * outstanding limit 7, variable burst, 4/8/16/32-beat bursts enabled.
 */
static void
program_dma_bus(YT * iv)
{
    uint32_t v = reg_read(iv, DMA_SBMR);

    v |= (1u << DMA_SBMR_EAME_POS);
    v = (v & ~(0xFu << DMA_SBMR_RD_OSR_LMT_POS)) | (0x7u << DMA_SBMR_RD_OSR_LMT_POS);
    v = (v & ~(0xFu << DMA_SBMR_WR_OSR_LMT_POS)) | (0x7u << DMA_SBMR_WR_OSR_LMT_POS);
    v &= ~(1u << DMA_SBMR_FB_POS);
    v |= (1u << DMA_SBMR_BLEN_4_POS) | (1u << DMA_SBMR_BLEN_8_POS)
       | (1u << DMA_SBMR_BLEN_16_POS) | (1u << DMA_SBMR_BLEN_32_POS);

    reg_write(iv, DMA_SBMR, v);
}

/*
 * DMA channel + MTL queue + MAC speed/duplex configuration.
 */
static kern_return_t
program_dma_and_mtl(YT * iv)
{
    uint32_t v;
    uint32_t hwf1 = reg_read(iv, MAC_HWF1R);
    uint32_t txFeat = (hwf1 >> MAC_HWF1R_TXFIFOSIZE_POS) & 0x1Fu;
    uint32_t rxFeat = (hwf1 >> MAC_HWF1R_RXFIFOSIZE_POS) & 0x1Fu;
    uint32_t tqs = calc_per_queue_fifo(txFeat, 1);
    uint32_t rqs = calc_per_queue_fifo(rxFeat, 1);

    /* Hardware capabilities and PHY interface status: read-only context
     * for debugging receive problems. */
    LOG("HW features HWF0R=0x%08x HWF2R=0x%08x HWF3R=0x%08x PHYIF_STA=0x%08x VR=0x%08x",
        reg_read(iv, MAC_HWF0R), reg_read(iv, MAC_HWF2R),
        reg_read(iv, MAC_HWF3R), reg_read(iv, MAC_PHYIF_STA),
        reg_read(iv, MAC_VR));

    /* RQC0R writability probe: write all 1s, read back, then write the
     * target value. */
    {
        uint32_t before = reg_read(iv, MAC_RQC0R);
        reg_write(iv, MAC_RQC0R, 0xFFFFFFFFu);
        uint32_t probe = reg_read(iv, MAC_RQC0R);
        reg_write(iv, MAC_RQC0R, before);
        LOG("RQC0R probe: original=0x%08x all-ones readback=0x%08x", before, probe);
    }

    LOG("HWF1R=0x%08x TXFIFO=2^%u RXFIFO=2^%u -> TQS=0x%x(%u KB) RQS=0x%x(%u KB)",
        hwf1, txFeat, rxFeat, tqs, (tqs + 1) / 4, rqs, (rqs + 1) / 4);

    program_dma_bus(iv);

    /* Burst length (PBL) and PBLX8: tx=16 / rx=4 / pblx8=1. A PBL of 0
     * is undefined, not "auto"; with PBLX8 the actual burst is PBLx8
     * beats (16 bytes per beat at 128-bit data width). */
    reg_write(iv, dma_ch_reg(DMA_CH_CR),
              reg_read(iv, dma_ch_reg(DMA_CH_CR)) | (1u << DMA_CH_CR_PBLX8_POS));
    reg_write(iv, dma_ch_reg(DMA_CH_TCR),
              (reg_read(iv, dma_ch_reg(DMA_CH_TCR))
               & ~(((1u << DMA_CH_TCR_PBL_LEN) - 1) << DMA_CH_TCR_PBL_POS))
              | ((16u & ((1u << DMA_CH_TCR_PBL_LEN) - 1)) << DMA_CH_TCR_PBL_POS));
    reg_write(iv, dma_ch_reg(DMA_CH_RCR),
              (reg_read(iv, dma_ch_reg(DMA_CH_RCR))
               & ~(((1u << DMA_CH_RCR_PBL_LEN) - 1) << DMA_CH_RCR_PBL_POS))
              | ((4u & ((1u << DMA_CH_RCR_PBL_LEN) - 1)) << DMA_CH_RCR_PBL_POS));

    /* TX queue config must be "disable, configure, then enable":
     * read-modify-write on an enabled queue silently drops some control
     * bits (e.g. TSF), leaving the queue half-configured so the MTL
     * FIFO never accepts data. */
    /* Flush the TX FIFO first (FTQ self-clears). Must happen before
     * disable+configure: flushing an enabled queue also clears TSF. */
    {
        uint32_t q = reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)) | (1u << MTL_Q_TQOMR_FTQ_POS);
        reg_write(iv, mtl_q_reg(0, MTL_Q_TQOMR), q);
        for (int i = 0; i < 50; i++) {
            IODelay(100);
            if ((reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)) & (1u << MTL_Q_TQOMR_FTQ_POS)) == 0) {
                break;
            }
        }
    }

    v = reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR));
    v &= ~(((1u << MTL_Q_TQOMR_TXQEN_LEN) - 1) << MTL_Q_TQOMR_TXQEN_POS);
    reg_write(iv, mtl_q_reg(0, MTL_Q_TQOMR), v);          /* 1. disable queue */
    LOG("TQOMR after queue off =0x%08x", reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)));

    v = reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR));
    v &= ~(((1u << MTL_Q_TQOMR_TQS_LEN) - 1) << MTL_Q_TQOMR_TQS_POS);
    v |= ((tqs & ((1u << MTL_Q_TQOMR_TQS_LEN) - 1)) << MTL_Q_TQOMR_TQS_POS);
    v |= (1u << MTL_Q_TQOMR_TSF_POS);       /* store-and-forward: whole frame in FIFO before TX */
    reg_write(iv, mtl_q_reg(0, MTL_Q_TQOMR), v);          /* 2. configure TQS/TSF */
    LOG("TQOMR after TQS/TSF =0x%08x (TSF should be 1)",
        reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)));

    v = reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR));
    v &= ~(((1u << MTL_Q_TQOMR_TXQEN_LEN) - 1) << MTL_Q_TQOMR_TXQEN_POS);
    v |= ((uint32_t)MTL_QUEUE_ENABLED << MTL_Q_TQOMR_TXQEN_POS);
    reg_write(iv, mtl_q_reg(0, MTL_Q_TQOMR), v);          /* 3. enable queue */
    LOG("TQOMR after queue on =0x%08x (TXQEN should be 0b10)",
        reg_read(iv, mtl_q_reg(0, MTL_Q_TQOMR)));

    v = reg_read(iv, mtl_q_reg(0, MTL_Q_RQOMR));
    v &= ~(((1u << MTL_Q_RQOMR_RQS_LEN) - 1) << MTL_Q_RQOMR_RQS_POS);
    v |= ((rqs & ((1u << MTL_Q_RQOMR_RQS_LEN) - 1)) << MTL_Q_RQOMR_RQS_POS);
    v |= (1u << MTL_Q_RQOMR_RSF_POS);       /* store-and-forward: whole frame before DMA */
    v |= (1u << MTL_Q_RQOMR_FEP_POS);       /* forward error frames, driver drops on ES */
    reg_write(iv, mtl_q_reg(0, MTL_Q_RQOMR), v);

    /* RX queue mapping: RXQ0EN must be 0b10 or frames are never routed
     * to any queue (see MAC_RQC0R comment in yt6801_reg.h). */
    v = reg_read(iv, MAC_RQC0R);
    v &= ~(((1u << MAC_RQC0R_RXQ0EN_LEN) - 1) << MAC_RQC0R_RXQ0EN_POS);
    v |= ((uint32_t)MAC_RXQ_ENABLED << MAC_RQC0R_RXQ0EN_POS);
    reg_write(iv, MAC_RQC0R, v);
    LOG("RQC0R RX queue mapping =0x%08x (RXQ0EN should be 0b10)", reg_read(iv, MAC_RQC0R));

    /* ACS=1 auto pad/short-frame + CRC strip, CST=1 CRC strip. */
    reg_write(iv, MAC_CR,
              reg_read(iv, MAC_CR)
              | (1u << MAC_CR_CST_POS) | (1u << MAC_CR_ACS_POS));

    /* Speed/duplex follows the link: 1000M is PS=0 FES=0 DM=1, 100M is
     * PS=1 FES=1 (PS=0 FES=1 is the 2500M encoding). */
    {
        bool fd = true;
        uint32_t spd = yt_sync_mac_speed(iv, &fd);
        LOG("MAC speed set %uM %s MAC_CR=0x%08x (PS/FES/DM: gigabit=0/0/1)",
            spd, fd ? "full duplex" : "half duplex", reg_read(iv, MAC_CR));
    }

    return kIOReturnSuccess;
}

kern_return_t
io_miniyv_yt6801ethernet::setupDatapath()
{
    YT * iv = ivars;
    kern_return_t ret;

    if (iv == nullptr || !iv->hardwareReady) {
        return kIOReturnNotReady;
    }
    if (iv->datapathReady) {
        return kIOReturnSuccess;        /* idempotent */
    }

    /* ---- 1. The two rings ---- */
    ret = alloc_dma_ring(this, iv,
                         YT6801_TX_RING_DESC * YT6801_DESC_SIZE,
                         &iv->txRingMem, &iv->txRingMap, &iv->txRingDma,
                         &iv->txDesc, &iv->txRingPhys);
    if (ret != kIOReturnSuccess) {
        goto fail;
    }

    ret = alloc_dma_ring(this, iv,
                         YT6801_RX_RING_DESC * YT6801_DESC_SIZE,
                         &iv->rxRingMem, &iv->rxRingMap, &iv->rxRingDma,
                         &iv->rxDesc, &iv->rxRingPhys);
    if (ret != kIOReturnSuccess) {
        goto fail;
    }

    /* TX data buffers: one 2 KB block per slot (see tx_submit_packet). */
    {
        uint32_t got = 0;
        for (uint32_t i = 0; i < YT6801_TX_RING_DESC; i++) {
            volatile uint32_t * base = nullptr;
            ret = alloc_dma_ring(this, iv, YT6801_RX_BUFFER_SIZE,
                                 &iv->txSlotMem[i], &iv->txSlotMap[i], &iv->txSlotDma[i],
                                 &base, &iv->txSlotPhys[i]);
            if (ret != kIOReturnSuccess) {
                LOG("TX buffers only allocated %u/%u segments", got, YT6801_TX_RING_DESC);
                break;
            }
            if (base != nullptr) {
                volatile uint8_t * p = (volatile uint8_t *)base;
                for (uint32_t k = 0; k < YT6801_RX_BUFFER_SIZE; k++) {
                    p[k] = 0;
                }
            }
            got++;
        }
        if (got == 0) {
            goto fail;
        }
        LOG("TX buffers ready: %u segments x %u bytes, first phys=0x%llx",
            got, (unsigned)YT6801_RX_BUFFER_SIZE,
            (unsigned long long)iv->txSlotPhys[0]);
    }

    /* RX data buffers: one 2 KB contiguous block per slot (see
     * rx_arm_descriptor). */
    {
        uint32_t got = 0;
        for (uint32_t i = 0; i < YT6801_RX_RING_DESC; i++) {
            volatile uint32_t * base = nullptr;
            ret = alloc_dma_ring(this, iv, YT6801_RX_BUFFER_SIZE,
                                 &iv->rxSlotMem[i], &iv->rxSlotMap[i], &iv->rxSlotDma[i],
                                 &base, &iv->rxSlotPhys[i]);
            if (ret != kIOReturnSuccess) {
                LOG("RX buffers only allocated %u/%u segments", got, YT6801_RX_RING_DESC);
                break;
            }
            /* Buffers must be zeroed: freshly allocated memory is dirty,
             * and a spurious completion with no data moved would deliver
             * garbage as a frame. Zeroing makes those EtherType 0, which
             * the second check reliably catches. */
            if (base != nullptr) {
                volatile uint8_t * p = (volatile uint8_t *)base;
                for (uint32_t k = 0; k < YT6801_RX_BUFFER_SIZE; k++) {
                    p[k] = 0;
                }
            }
            got++;
        }
        if (got == 0) {
            goto fail;
        }
        LOG("RX buffers ready: %u segments x %u bytes, first phys=0x%llx",
            got, (unsigned)YT6801_RX_BUFFER_SIZE,
            (unsigned long long)iv->rxSlotPhys[0]);
    }

    /* Ring memory may be dirty: clear it, one stale OWN=1 descriptor
     * would make the hardware DMA to a wild address. */
    for (uint32_t i = 0; i < YT6801_TX_RING_DESC * YT6801_DESC_WORDS; i++) {
        iv->txDesc[i] = 0;
    }
    for (uint32_t i = 0; i < YT6801_RX_RING_DESC * YT6801_DESC_WORDS; i++) {
        iv->rxDesc[i] = 0;
    }
    OSSynchronizeIO();

    iv->txHead = iv->txClean = 0;
    iv->rxArm  = iv->rxClean = 0;
    iv->rxPending = 0;
    iv->rxFreeCount = 0;

    /* ---- 2. Ring base and length ---- */
    reg_write(iv, dma_ch_reg(DMA_CH_TDLR_LO), (uint32_t)(iv->txRingPhys & 0xFFFFFFFFull));
    reg_write(iv, dma_ch_reg(DMA_CH_TDLR_HI), (uint32_t)(iv->txRingPhys >> 32));
    reg_write(iv, dma_ch_reg(DMA_CH_TDRLR), YT6801_TX_RING_DESC - 1);

    reg_write(iv, dma_ch_reg(DMA_CH_RDLR_LO), (uint32_t)(iv->rxRingPhys & 0xFFFFFFFFull));
    reg_write(iv, dma_ch_reg(DMA_CH_RDLR_HI), (uint32_t)(iv->rxRingPhys >> 32));
    reg_write(iv, dma_ch_reg(DMA_CH_RDRLR), YT6801_RX_RING_DESC - 1);

    /* Read back immediately: these registers are the only way the DMA
     * finds the ring; if a write did not stick, everything after is
     * pointless and must be visible in the log. */
    LOG("ring base readback TDLR=0x%08x TDRLR=%u RDLR=0x%08x RDRLR=%u (wrote 0x%llx/0x%llx)",
        reg_read(iv, dma_ch_reg(DMA_CH_TDLR_LO)),
        reg_read(iv, dma_ch_reg(DMA_CH_TDRLR)),
        reg_read(iv, dma_ch_reg(DMA_CH_RDLR_LO)),
        reg_read(iv, dma_ch_reg(DMA_CH_RDRLR)),
        (unsigned long long)iv->txRingPhys,
        (unsigned long long)iv->rxRingPhys);

    /* ---- 3. DMA channel / MTL ---- */
    ret = program_dma_and_mtl(iv);
    if (ret != kIOReturnSuccess) {
        goto fail;
    }

    /* ---- 4. RX descriptors: clear and start at 0. The channel is
     * restarted by enableDatapath, which resets CARDR to slot 0; arming
     * from slot 0 keeps the hardware fetch and the driver cursor in
     * step. ---- */
    for (uint32_t i = 0; i < YT6801_RX_RING_DESC * YT6801_DESC_WORDS; i++) {
        iv->rxDesc[i] = 0;
    }
    OSSynchronizeIO();
    iv->rxClean = 0;
    iv->rxArm   = 0;
    iv->rxPending = 0;

    /* ---- 5. Initial RX buffers: arm all but one slot ----
     * Do not infer "how many armed" from rxArm: arming the last slot
     * wraps rxArm back to 0, indistinguishable from none armed. */
    {
        uint32_t armed = 0;
        /* Force arming from slot 0: enableDatapath restarts the channel,
         * resetting CARDR to slot 0 — DMA would stall on an empty slot
         * 0 and fetch nothing. */
        iv->rxArm = 0;
        iv->rxClean = 0;
        iv->rxPending = 0;
        for (uint32_t n = 0; n < YT6801_RX_RING_DESC - 1; n++) {
            IOUserNetworkPacket * pkt = nullptr;
            if (iv->pool->allocatePacket(&pkt) != kIOReturnSuccess || pkt == nullptr) {
                LOG("RX initial arm got only %u/%u packets, pool too small or busy", n, YT6801_RX_RING_DESC);
                break;
            }
            /* Bare packets must be prepared to get a writable DMA address. */
            (void)rx_prepare_packet(iv, pkt);
            rx_arm_descriptor(iv, iv->rxArm, pkt);
            iv->rxArm = ring_next(iv->rxArm, YT6801_RX_RING_DESC);
            iv->rxPending++;
            armed++;
        }
        if (armed == 0) {
            LOG("no RX buffers armed, datapath cannot work");
            ret = kIOReturnNoMemory;
            goto fail;
        }

        pushRxTail(iv);
        LOG("RX ring armed %u buffers, tail pushed to descriptor %u (must differ from CARDR=0)",
            armed, ring_prev(iv->rxArm, YT6801_RX_RING_DESC));
    }

    /* ---- 6. Interrupts ---- */
    /* MSI-X first: MSI returns kIOReturnUnsupported on this card, but
     * the MSI-X capability exists. Failure on both is not fatal: rings,
     * doorbells and DMA work independently. */
    {
        static const uint32_t irqTypes[] = {
            kIOInterruptTypePCIMessagedX,
            kIOInterruptTypePCIMessaged,
        };
        kern_return_t irqRet = kIOReturnUnsupported;

        for (unsigned k = 0; k < sizeof(irqTypes) / sizeof(irqTypes[0]); k++) {
            irqRet = iv->pciDevice->ConfigureInterrupts(irqTypes[k], 1, 1, 0);
            LOG("ConfigureInterrupts(type=0x%x) -> 0x%x", irqTypes[k], irqRet);
            if (irqRet == kIOReturnSuccess) {
                iv->irqMode = irqTypes[k];
                break;
            }
        }

        if (irqRet == kIOReturnSuccess) {
            ret = IOInterruptDispatchSource::Create(iv->pciDevice, 0,
                                                    iv->dispatchQueue, &iv->irqSource);
            if (ret == kIOReturnSuccess && iv->irqSource != nullptr) {
                ret = CreateActionInterruptOccurred(0, &iv->irqAction);
                if (ret == kIOReturnSuccess && iv->irqAction != nullptr) {
                    ret = iv->irqSource->SetHandler(iv->irqAction);
                }
            }
            if (ret != kIOReturnSuccess) {
                LOG("interrupt dispatch source setup failed 0x%x (no-interrupt fallback)", ret);
                OSSafeReleaseNULL(iv->irqSource);
                iv->irqMode = 0;
            }
        } else {
            LOG("MSI/MSI-X unavailable, no-interrupt mode (TX doorbell still active)");
        }
        ret = kIOReturnSuccess;
    }

    ret = CreateActionTxDataAvailable(0, &iv->txDoorbellAction);
    if (ret != kIOReturnSuccess || iv->txDoorbellAction == nullptr) {
        LOG("TX doorbell action create failed 0x%x", ret);
        goto fail;
    }
    ret = iv->txSubmit->CopyDataQueue(&iv->txDataQueue);
    if (ret != kIOReturnSuccess || iv->txDataQueue == nullptr) {
        LOG("TX data queue copy failed 0x%x", ret);
        goto fail;
    }
    ret = iv->txDataQueue->SetDataAvailableHandler(iv->txDoorbellAction);
    if (ret != kIOReturnSuccess) {
        LOG("TX doorbell handler set failed 0x%x", ret);
        goto fail;
    }
    /* Setting the handler is not enough: data queue event sources are
     * disabled by default and the doorbell never fires until enabled. */
    ret = iv->txDataQueue->SetEnableWithCompletion(true, nullptr);
    if (ret != kIOReturnSuccess) {
        LOG("TX doorbell enable failed 0x%x", ret);
        goto fail;
    }

    ret = CreateActionRxDataAvailable(0, &iv->rxDoorbellAction);
    if (ret != kIOReturnSuccess || iv->rxDoorbellAction == nullptr) {
        LOG("RX doorbell action create failed 0x%x", ret);
        goto fail;
    }
    ret = iv->rxSubmit->CopyDataQueue(&iv->rxDataQueue);
    if (ret != kIOReturnSuccess || iv->rxDataQueue == nullptr) {
        LOG("RX data queue copy failed 0x%x", ret);
        goto fail;
    }
    ret = iv->rxDataQueue->SetDataAvailableHandler(iv->rxDoorbellAction);
    if (ret != kIOReturnSuccess) {
        LOG("RX doorbell handler set failed 0x%x", ret);
        goto fail;
    }
    ret = iv->rxDataQueue->SetEnableWithCompletion(true, nullptr);
    if (ret != kIOReturnSuccess) {
        LOG("RX doorbell enable failed 0x%x", ret);
        goto fail;
    }

    if (iv->irqSource != nullptr) {
        ret = iv->irqSource->SetEnableWithCompletion(true, nullptr);
        if (ret != kIOReturnSuccess) {
            LOG("interrupt enable failed 0x%x (doorbell path unaffected)", ret);
        }
    }

    iv->datapathReady = true;
    LOG("datapath ready: TX %u descriptors, RX %u descriptors, RX armed %u buffers (rxArm=%u)",
        (unsigned)YT6801_TX_RING_DESC, (unsigned)YT6801_RX_RING_DESC,
        iv->rxPending, iv->rxArm);

    /* The stack may have called SetInterfaceEnable(true) before
     * setupDatapath (bringing the interface up right after registration
     * is the norm). That call only set MAC RE/TE because datapathReady
     * was still false; start the DMA here or Opkts stays 0 forever. */
    if (iv->interfaceEnabled) {
        enableDatapath(true);
        LOG("datapath started per earlier interfaceEnabled state");
    }

    return kIOReturnSuccess;

fail:
    teardownDatapath();
    return ret;
}

void
io_miniyv_yt6801ethernet::teardownDatapath()
{
    YT * iv = ivars;

    if (iv == nullptr) {
        return;
    }

    /* Stop the hardware before touching memory, or the DMA writes into
     * freed pages. */
    if (iv->datapathReady) {
        reg_write(iv, MAC_CR, reg_read(iv, MAC_CR)
                  & ~((1u << MAC_CR_RE_POS) | (1u << MAC_CR_TE_POS)));
        reg_write(iv, dma_ch_reg(DMA_CH_IER), 0);
        reg_write(iv, dma_ch_reg(DMA_CH_RCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_RCR)) & ~(1u << DMA_CH_RCR_SR_POS));
        reg_write(iv, dma_ch_reg(DMA_CH_TCR),
                  reg_read(iv, dma_ch_reg(DMA_CH_TCR)) & ~(1u << DMA_CH_TCR_ST_POS));
    }

    if (iv->irqSource != nullptr) {
        iv->irqSource->SetEnableWithCompletion(false, nullptr);
        iv->irqSource->Cancel(nullptr);
    }
    OSSafeReleaseNULL(iv->irqSource);
    OSSafeReleaseNULL(iv->irqAction);
    OSSafeReleaseNULL(iv->txDoorbellAction);
    OSSafeReleaseNULL(iv->rxDoorbellAction);
    OSSafeReleaseNULL(iv->txDataQueue);
    OSSafeReleaseNULL(iv->rxDataQueue);

    /* Return all in-flight packets or the pool permanently shrinks. */
    if (iv->datapathReady) {
        for (uint32_t i = 0; i < YT6801_TX_RING_DESC; i++) {
            if (iv->txPackets[i] != nullptr) {
                tx_return_packet(iv, iv->txPackets[i]);
                iv->txPackets[i] = nullptr;
            }
        }
        for (uint32_t i = 0; i < YT6801_RX_RING_DESC; i++) {
            if (iv->rxPackets[i] != nullptr) {
                iv->pool->deallocatePacket(iv->rxPackets[i]);
                iv->rxPackets[i] = nullptr;
            }
        }
        for (uint32_t i = 0; i < iv->rxFreeCount; i++) {
            iv->pool->deallocatePacket(iv->rxFree[i]);
            iv->rxFree[i] = nullptr;
        }
        iv->rxFreeCount = 0;
    }

    free_dma_ring(&iv->txRingMem, &iv->txRingMap, &iv->txRingDma);
    free_dma_ring(&iv->rxRingMem, &iv->rxRingMap, &iv->rxRingDma);
    for (uint32_t i = 0; i < YT6801_TX_RING_DESC; i++) {
        free_dma_ring(&iv->txSlotMem[i], &iv->txSlotMap[i], &iv->txSlotDma[i]);
        iv->txSlotPhys[i] = 0;
    }
    for (uint32_t i = 0; i < YT6801_RX_RING_DESC; i++) {
        free_dma_ring(&iv->rxSlotMem[i], &iv->rxSlotMap[i], &iv->rxSlotDma[i]);
        iv->rxSlotPhys[i] = 0;
    }

    iv->txDesc = nullptr;
    iv->rxDesc = nullptr;
    iv->txRingPhys = 0;
    iv->rxRingPhys = 0;
    iv->txHead = iv->txClean = iv->rxArm = iv->rxClean = 0;
    iv->rxPending = 0;
    iv->datapathReady = false;
}

void
io_miniyv_yt6801ethernet::logCounters(uint32_t why)
{
    YT * iv = ivars;

    if (iv == nullptr) {
        return;
    }
    LOG("stats[%u]: tx=%u rx=%u txFail=%u txNoSpace=%u txLong=%u rxErr=%u rxDrop=%u rxNoPkt=%u rxGhost=%u",
        why, iv->txFrames, iv->rxFrames, iv->txFailed, iv->txNoSpace, iv->txTooLong,
        iv->rxErrors, iv->rxDropped, iv->rxNoPacket, iv->rxFakeFrames);
}
