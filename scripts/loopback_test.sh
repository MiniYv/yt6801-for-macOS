#!/bin/bash
# yt6801 loopback test (unattended)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT/logs"
BIN="$ROOT/build/yt6801_host_verify"
DEXT="$ROOT/build/yt6801.dext"
mkdir -p "$LOG"

TS() { date "+%Y-%m-%d %H:%M:%S"; }
record() { echo "$(TS) $1 $2" >> "$LOG/test_results.log"; }

echo "===== yt6801 loopback test $(TS) ====="

if [ -d "$DEXT" ]; then
  echo "[1/3] Found DEXT: $DEXT"
  if systemextensionsctl list 2>/dev/null | grep -qi yt6801; then
    echo "      DEXT registered"
  else
    echo "      DEXT not registered (local ad-hoc load requires SIP disabled + systemextensionsctl developer on)"
  fi
else
  echo "[1/3] No DEXT found (skipping load; host-side verification mode)"
fi

if [ ! -x "$BIN" ]; then
  echo "[2/3] Compiling verification program..."
  clang -O2 -Wall -o "$BIN" "$ROOT/source/host/yt6801_host_verify.c" || { record FAIL "compile"; exit 1; }
fi

echo "[3/3] Running loopback self-test..."
"$BIN" "$LOG"
RC=$?

if [ $RC -eq 0 ] && grep -q "Loopback test passed" "$LOG"/driver_*.log 2>/dev/null; then
  echo "Result: PASS"
  record PASS "loopback"
  exit 0
else
  echo "Result: FAIL"
  record FAIL "loopback"
  exit 1
fi
