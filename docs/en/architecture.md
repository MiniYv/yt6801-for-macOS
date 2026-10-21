# Architecture

## What it is

A user-space DriverKit (DEXT) driver that takes over the Motorcomm YT6801
PCIe Gigabit Ethernet controller (`1f0a:6801`) and registers it with the macOS
network stack (Skywalk) as a regular `enX` Ethernet interface. No kernel code
runs: the driver lives in its own process, talks to the chip over mapped PCIe
BARs, and a crash only restarts the process — the kernel is unaffected.

Ported from Motorcomm's Linux driver (`fuxi-gmac`), inheriting GPL-2.0+.

## Stack

```
macOS network stack (Skywalk / IOUserNetworkEthernet)
        |  four queues (TX/RX submission + completion) + packet pool
dext process (user space)
   - Start/Stop, interface registration, link reporting
   - hardware init: BAR0+0x2000 register block, MDIO/PHY, DMA reset
   - data plane: doorbell-driven TX/RX over descriptor rings
        |  PCIe
YT6801 MAC (DWC-compatible) -> PHY
```

## Register layout

- MAC/DMA/MTL/PHY registers are relative to **BAR0 + 0x2000**
  (`FXGMAC_MAC_REGS_OFFSET` in the Linux source)
- The MGMT block (PHY reset line, EFUSE at 0x1500) is relative to **BAR0
  directly** — no offset

## DMA design: self-managed buffers

The critical design decision: the packet-pool IOVA handed out by
`IOUserNetworkPacket::getDataIOVirtualAddress()` does **not** work on this
MAC — descriptor writebacks and packet lengths look perfectly normal, but the
payload DMA reads/writes is unrelated memory. Both directions therefore use
self-managed 2 KB buffers (128 slots per direction, ~512 KB total) mapped via
`IODMACommand`, with one extra memcpy per frame in each direction. Descriptors
only ever reference `txSlotPhys[i]` / `rxSlotPhys[i]`.

Ring requirements: physically contiguous (single-segment DMA), 64-bit
addressing (`maxAddressBits = 64`), rings never cross a 4 GB boundary (4 KB
page alignment satisfies this since only the 32-bit TDTR/RDTR exist).

## Boot sequence

1. PCI match (`IOPCIPrimaryMatch 0x68011f0a`), BAR0 map
2. Chip reset reloads the EFUSE patch table and releases the PHY reset line
3. MAC address: EFUSE patch table → MACA0 register → static fallback
4. PHY autonegotiation wait (1–2 s hot plug, up to 15 s cold boot)
5. Loopback self-test only when no cable is detected; with a cable the
   datapath starts straight in real mode and the MAC speed is set from the
   negotiated rate
6. Descriptor rings started in stopped state (rings written before ST/SR),
   RX tail pushed after SR

## Link handling

MSI-X does not deliver on the tested hardware (`irqCount` stays 0), so link
state and RX are maintained by a 5 ms timer (200 ms effective poll cadence;
2 s in loopback). Unplug is detected by polling and reported to the stack,
which falls back to Wi-Fi; the MAC PS/FES/DM bits are re-synced on every rate
change.

## Implemented / not implemented

Working (verified on hardware):

- End-to-end TX/RX with correct frame contents, gigabit link, DHCP
- EFUSE real MAC address, multicast hash filtering (IPv6 ND works)
- Unplug detection and automatic fallback, sleep/wake survival

Not implemented (reported as unsupported, no fake claims):

- Hardware checksum offload, TSO, VLAN offload
- Multi-segment descriptors, multi-queue/RSS, interrupt coalescing
- MSI-X interrupts (timer polling instead)

Known cosmetic issue: `ifconfig` may print `SIOCGIFXMEDIA: Input/output
error` — the media table is pushed proactively but the legacy query path is
never called by the stack. It does not affect traffic.
