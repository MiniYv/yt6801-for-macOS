# Debugging Notes

Non-obvious facts that cost real debugging time. Code comments only say
"what"; this file records "why".

## Registers and bus

- MAC registers are relative to BAR0 + 0x2000. Symptom of missing the offset:
  `MAC_VR` reads 0xfee00000, MDIO stays busy forever, MAC address reads
  01:00:00:00:01:00. The MGMT block (PHY reset 0x1004, EFUSE 0x1500) takes no
  offset.
- `MemoryRead32`/`Write32` in PCIDriverKit return void and read all-ones
  (0xFFFFFFFF) on error — register access always "succeeds". An MDIO read of
  all-ones wedges the busy bit until the timeout path bails out.
- Chip-level reset (SYS_RESET_REG 0x152c bit31, self-clearing) reloads the
  EFUSE patch table, re-asserts the PHY reset line, and wipes non-sticky
  registers in BAR0 0x1000–0x1300 (save/restore them).
- While the PHY reset line (0x1004) is held in reset, MDIO still answers but
  the PHY outputs no MII clock — DMA stalls with no frames on the wire.
  Release sequence: write 0 → 1.5 ms → write 1.
- A PHY soft reset wipes all extended registers (AFE configuration); rewrite
  them after every soft reset.
- SPEC_STATUS layout: bit10 link, bit13 duplex, bits14:15 speed. Bit 11 is a
  different register's definition — misapplying it reads 100M as 1000M.

## MAC address

- Three-level source: EFUSE patch table → MACA0 register → static fallback.
  All-zero/all-FF/multicast addresses are invalid and fall through.
- The final address must be written back to the MAC address filter 0
  (MACA0LR/HR) — a stale illegal value there makes the MAC drop every
  incoming frame ("RX never receives").
- EFUSE row read: write row+START to CTRL_0, poll CTRL_1 DONE, data in
  CTRL_1 bits31:24.

## Descriptors and DMA

- Descriptors must point at driver-owned `IODMACommand` buffers, never at
  packet-pool IOVAs (see architecture.md). Ghost frames (OWN cleared, PL
  filled, payload unchanged) must be dropped; EtherType 0 is another dirty
  buffer signature.
- The packet pool needs both `PoolFlagMapToDext` and `PoolFlagToDevice`.
- Descriptors rings must be physically contiguous.
- RDES2 B1L must equal the buffer size; RCR.RBSZ does not replace it.
- desc3 FL must be written for TX; B1L alone is not enough.
- TX length limit is mtu + 14 + 4; the stack may deliver aggregated packets.
- Write OWN last, after `OSSynchronizeIO()`.

## Ring state machine

- Ring full/empty must use an explicit pending counter, not pointer equality
  (wrapping makes equal pointers look like "empty").
- Hang at most RING-2 descriptors; the RX tail pointer wrapping back to the
  ring base reads as "ring empty" to the DMA.
- Push the RX tail only after SR is set; starting the channel resets
  CARDR/RDTR to the ring head.
- Write ring base addresses while the channel is stopped, then set ST/SR.
- Deadlock triage: check CATDR/CARDR first — a stationary pointer means the
  DMA is not running; do not start by reading descriptor contents.
- Loss accounting: use the MMC hardware counters; MMC advancing with zero
  CRC errors means the wire is clean and loss is upstream.

## Interrupts and timing

- MSI-X returns success but never delivers on this hardware; PacketPoller
  queue binding is also refused. A 5 ms `IOTimerDispatchSource` handles
  collection instead.
- DriverKit MMIO is much faster than Linux; MDIO polling needs explicit
  `IODelay(10)` loops or it does not wait at all.

## Link

- Decide loopback vs real mode before any PHY operation: the self-test's soft
  resets tear down an established link.
- PHY autonegotiation runs in the background: 1–2 s for hot plug, 15 s+ for
  cold boot.
- Do not hardcode MAC speed 1000M; program PS/FES/DM from the negotiated
  rate or a 100M link shows "up" but passes no traffic.
- Read ANLPAR/STAT1000 only after the link is up (reads 0 within 1 s of
  reset).

## macOS stack

- The stack never calls `getSupportedMediaArray`; push media types with
  `ReportAvailableMediaTypes` after registration.
- Reporting link too early inside `Start` is dropped; re-report from
  `SetInterfaceEnable` (triggered by `ifconfig up/down`).
- All four packet queues need an explicit `setEnable` or the stack stops
  delivering.
