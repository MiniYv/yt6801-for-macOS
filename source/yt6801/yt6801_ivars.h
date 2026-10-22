/*
 * yt6801_ivars.h — driver instance state
 *
 * The iig-generated yt6801.h already forward-declares
 * io_miniyv_yt6801ethernet_IVars and injects the
 * ivars/lvars union member, so this header only provides the full
 * definition and must be included after "yt6801.h".
 *
 * State lives in ivars (not class members) because the class layout is
 * iig-generated and participates in cross-process descriptors; ivars is
 * the only heap memory fully owned by the driver, aligned with init/free.
 */
#ifndef YT6801_IVARS_H
#define YT6801_IVARS_H

#include <DriverKit/DriverKit.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>

#include "yt6801_reg.h"   /* ring sizes and register semantics together */

#define YT6801_MAC_LEN          6

/* ============================================================
 * Datapath
 *
 * Ring sizes must be powers of two: TDTR/RDTR are device-side
 * address pointers and wraparound relies on masking.
 * ============================================================ */
#if (YT6801_TX_RING_DESC & (YT6801_TX_RING_DESC - 1)) != 0
#error "YT6801_TX_RING_DESC must be a power of two"
#endif
#if (YT6801_RX_RING_DESC & (YT6801_RX_RING_DESC - 1)) != 0
#error "YT6801_RX_RING_DESC must be a power of two"
#endif

/* Queue ids: two submission directions share 0, completions use 4 and 5. */
#define YT6801_QID_TX_SUBMIT    0
#define YT6801_QID_RX_SUBMIT    0
#define YT6801_QID_TX_COMPLETE  4
#define YT6801_QID_RX_COMPLETE  5

struct io_miniyv_yt6801ethernet_IVars {
    /* ---- PCI ---- */
    IOPCIDevice *  pciDevice;
    uint8_t        barIndex;
    uint64_t       barSize;
    uint8_t        barType;
    bool           pciOpened;

    /* ---- PHY ---- */
    uint16_t       phyId1;
    uint16_t       phyId2;

    /* ---- Identity ---- */
    uint8_t        macAddress[YT6801_MAC_LEN];
    bool           macValid;

    /* ---- State ---- */
    bool           loopbackActive;
    /* Timestamp (ns) of the current loopback-to-real probe window; 0 = not probing. */
    uint64_t       loopbackProbeStart;
    /* Whether to put the PHY into loopback on this boot. Must be decided
     * before setupDatapath: once in loopback the link bit stays 0. */
    bool           wantLoopback;
    bool           hardwareReady;
    /* Latest PHY negotiation result; MAC speed/duplex follows it. */
    uint32_t       linkSpeedMbps;
    bool           linkFullDuplex;
    bool           interfaceEnabled;
    bool           linkUp;
    uint32_t       mtu;

    /* ---- Counters (diagnostics) ---- */
    uint32_t       mdioReads;
    uint32_t       mdioWrites;
    uint32_t       txFrames;
    uint32_t       rxFrames;

    /* ---- Network stack resources ---- */
    IODispatchQueue *                dispatchQueue;
    IOUserNetworkPacketBufferPool *  pool;
    IOUserNetworkTxSubmissionQueue * txSubmit;
    IOUserNetworkTxCompletionQueue * txComplete;
    IOUserNetworkRxSubmissionQueue * rxSubmit;
    IOUserNetworkRxCompletionQueue * rxComplete;
    IOUserNetworkPacketPoller *      packetPoller;
    IOTimerDispatchSource *          pollTimer;
    uint32_t                         rxHarvested;    /* frames harvested by the last serviceRx call */
    uint32_t                         pollIdle;       /* consecutive idle poll ticks */
    bool                             netRegistered;

    /* ---- Interrupts and doorbells ---- */
    uint32_t                    irqMode;        /* 0 = no interrupts; otherwise the ConfigureInterrupts type */
    IOInterruptDispatchSource * irqSource;
    OSAction *                  irqAction;
    OSAction *                  txDoorbellAction;
    OSAction *                  rxDoorbellAction;
    /* The queue itself is not an event source; the data queue is where
     * the doorbell hangs. */
    IODataQueueDispatchSource * txDataQueue;
    IODataQueueDispatchSource * rxDataQueue;

    /* ---- Descriptor rings (DMA-visible memory) ---- */
    IOBufferMemoryDescriptor * txRingMem;
    IOMemoryMap *              txRingMap;
    IODMACommand *             txRingDma;
    volatile uint32_t *        txDesc;      /* CPU virtual address of the ring */
    uint64_t                   txRingPhys;  /* device-visible ring base */

    IOBufferMemoryDescriptor * rxRingMem;
    IOMemoryMap *              rxRingMap;
    IODMACommand *             rxRingDma;
    volatile uint32_t *        rxDesc;
    uint64_t                   rxRingPhys;

    /* ---- Ring cursors ----
     * TX: [txClean, txHead) handed to hardware; txHead is next to fill.
     * RX: [rxClean, rxArm) handed to hardware; rxArm is next to arm.
     * Both rings run on one dispatch queue, no locking needed. */
    uint32_t       txHead;
    uint32_t       txClean;
    uint32_t       rxArm;
    uint32_t       rxClean;

    /* In-flight: txPackets[i] is owned by descriptor i, cleared on reclaim. */
    IOUserNetworkPacket * txPackets[YT6801_TX_RING_DESC];
    /* Packets armed on the RX ring: handed to the stack after harvest. */
    IOUserNetworkPacket * rxPackets[YT6801_RX_RING_DESC];

    /* Staging for RX packets returned by the stack, used by refillRx. */
    IOUserNetworkPacket * rxFree[YT6801_RX_RING_DESC];
    uint32_t              rxFreeCount;
    uint32_t              rxPending;     /* descriptors owned by hardware (true length of rxClean..rxArm) */

    /* ---- Driver-allocated RX DMA buffers (see rx_arm_descriptor) ----
     * Pool packets' getDataIOVirtualAddress() is not writable by the
     * hardware, so RX always DMAs into this contiguous memory and is
     * then memcpy'd into the pool packet. */
    /* One 2 KB block per RX slot: a single 256 KB allocation would be
     * split into many physical segments; 2 KB fits in one 4 KB page. */
    IOBufferMemoryDescriptor * rxSlotMem[YT6801_RX_RING_DESC];
    IOMemoryMap *              rxSlotMap[YT6801_RX_RING_DESC];
    IODMACommand *             rxSlotDma[YT6801_RX_RING_DESC];
    uint64_t                   rxSlotPhys[YT6801_RX_RING_DESC];

    /* ---- Driver-allocated TX DMA buffers ----
     * Same reason as RX: the pool packet's IOVA does not read back the
     * data the stack wrote, so data is memcpy'd here before transmit
     * and the descriptor points at this buffer's device address. */
    IOBufferMemoryDescriptor * txSlotMem[YT6801_TX_RING_DESC];
    IOMemoryMap *              txSlotMap[YT6801_TX_RING_DESC];
    IODMACommand *             txSlotDma[YT6801_TX_RING_DESC];
    uint64_t                   txSlotPhys[YT6801_TX_RING_DESC];

    /* ---- Runtime state ---- */
    bool           datapathReady;
    bool           txServicing;     /* guards serviceTx re-entry */
    uint32_t       txNoSpace;
    uint32_t       txFailed;
    uint32_t       txTooLong;       /* aggregates over MTU, rejected */
    uint32_t       rxErrors;
    uint32_t       rxDropped;
    uint32_t       rxNoPacket;      /* pool exhausted */
    uint32_t       rxFrameCount;    /* frames delivered to the stack */
    uint32_t       rxFrameLogged;   /* frames detail-logged, capped at 8 */
    uint32_t       txServiceCount;  /* serviceTx invocations */
    uint32_t       doorbellCount;   /* TX doorbell fires */
    uint32_t       rxProbeCount;    /* RX path probe prints */
    uint32_t       rxFakeFrames;    /* ghost frames rejected (short PL or EtherType 0) */
    uint32_t       rxFakeLogged;    /* ghost frames logged, capped at 4 */
    uint32_t       irqCount;        /* hardware interrupts delivered */
    bool           selfLoopMode;    /* no cable: PHY loopback held, link reported UP */

    /* ---- Loopback self-test result bitmap, 0 = all passed ---- */
    uint64_t       loopbackResult;

    /* ---- Hardware loopback window ----
     * Places the PHY in internal loopback for a while so stack-sent
     * frames come straight back; observable via netstat without a cable.
     * The PHY must be restored unconditionally when the window ends. */
    IOTimerDispatchSource * loopbackTimer;
    OSAction *              loopbackTimerAction;
    uint16_t                bmcrSaved;          /* BMCR before entering loopback */
    uint16_t                loopbackBmcr;       /* BMCR value that keeps loopback */
    bool                    loopbackWindowOpen;
    uint64_t                hwLoopbackResult;   /* bitmap: 0 = frame really came back */
    uint32_t                hwLoopbackTxLen;
    uint32_t                hwLoopbackRxLen;
    uint32_t                linkReports;        /* link status reports sent */
    uint32_t                linkPollTick;       /* 5ms ticks since last link poll */
};

#endif /* YT6801_IVARS_H */
