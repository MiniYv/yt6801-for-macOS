#!/bin/bash
#
# test.sh - All verification that runs without loading the driver
#
# 1. yt6801_host_verify: runs the driver's exact loopback self-test sequence
#    against a virtual MAC/PHY with YT6801 register semantics. Proves the
#    ported register operations and MDIO timing are self-consistent, not that
#    real hardware works (that needs the loaded dext + os_log).
#
# 2. yt6801_ring_sim: pure descriptor-ring index arithmetic, hardware-
#    independent (200k random ops). Guarantees no descriptor still held by
#    hardware is overwritten and no packets are dropped or duplicated.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CC=/usr/bin/clang
FAIL=0

green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
line()  { printf '%s\n' "------------------------------------------------------------"; }

mkdir -p build

line
echo "[1/2] PHY loopback self-test (registers + MDIO timing, virtual hardware)"
if $CC -O2 -Wall -o build/yt6801_host_verify source/host/yt6801_host_verify.c; then
    if ./build/yt6801_host_verify; then
        green "  PASS"
    else
        red "  FAIL"; FAIL=1
    fi
else
    red "  Compile failed"; FAIL=1
fi

line
echo "[2/2] Descriptor ring state machine (index arithmetic, 200k random ops)"
if $CC -O2 -Wall -o build/yt6801_ring_sim source/host/yt6801_ring_sim.c; then
    if ./build/yt6801_ring_sim; then
        green "  PASS"
    else
        red "  FAIL"; FAIL=1
    fi
else
    red "  Compile failed"; FAIL=1
fi

line
if [ $FAIL -eq 0 ]; then
    green "All passed"
else
    red "Some tests failed"
fi
exit $FAIL
