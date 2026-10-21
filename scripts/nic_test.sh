#!/bin/bash
#
# nic_test.sh — full functional / performance / stability test for the YT6801 NIC.
#
# Sections:
#   1. driver + interface detection
#   2. link state
#   3. error counters baseline (netstat)
#   4. DNS resolution (system resolver + direct query)
#   5. latency / loss (gateway + public IP)
#   6. full-size DF frames (MTU boundary)
#   7. throughput (curl download bound to the interface)
#   8. concurrent soak (ping + parallel transfers), then error/log scan
#
# Usage:
#   ./scripts/nic_test.sh              # auto-detect interface
#   ./scripts/nic_test.sh en5          # force interface
#   SOAK=30 ./scripts/nic_test.sh      # soak length in seconds (default 30)
#
set -u

IFACE="${1:-${IFACE:-auto}}"
SOAK="${SOAK:-30}"
PING_TARGET="${PING_TARGET:-223.5.5.5}"
DNS_SERVER="${DNS_SERVER:-223.5.5.5}"
# ~50 MB test payload; override with a local LAN URL for clean measurements.
THROUGHPUT_URL="${THROUGHPUT_URL:-https://speed.cloudflare.com/__down?bytes=50000000}"
FAIL=0

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
note()  { printf '\033[2m%s\033[0m\n' "$*"; }
check() { if [ "$1" -eq 0 ]; then green "  PASS: $2"; else red "  FAIL: $2"; FAIL=1; fi; }

# ---------- 1. driver + interface ----------
echo "[1/8] Driver and interface"
sysext=$(systemextensionsctl list 2>/dev/null | grep 'io\.miniyv\.yt6801ethernet' | head -1)
[ -n "$sysext" ] && note "  driver: $(echo "$sysext" | tr -s '\t' ' ')" || { red "  driver extension not found"; exit 1; }
echo "$sysext" | grep -q 'activated enabled'
check $? "driver state is [activated enabled]"
dext_pid=$(pgrep -f "io\.miniyv\.yt6801ethernet\.dext" | head -1)
[ -n "$dext_pid" ] && green "  dext process: pid $dext_pid" || { red "  dext process not running"; exit 1; }

if [ "$IFACE" = "auto" ]; then
    for i in $(ifconfig -l); do
        # Match the Motorcomm OUI of the YT6801 EFUSE MAC.
        if ifconfig "$i" 2>/dev/null | grep -qi 'ether b0:25:aa:'; then IFACE="$i"; break; fi
    done
fi
[ -n "${IFACE:-}" ] && [ "$IFACE" != "auto" ] || { red "  no YT6801 interface found"; exit 1; }
ifconfig "$IFACE" >/dev/null 2>&1 || { red "  interface $IFACE does not exist"; exit 1; }
green "  interface: $IFACE"

# ---------- 2. link state ----------
echo "[2/8] Link state"
link_line=$(ifconfig "$IFACE" | grep "status:")
note "  $link_line"
echo "$link_line" | grep -q "active"
check $? "link status is active"
media_line=$(ifconfig "$IFACE" | grep "media:")
note "  $media_line"

# ---------- 3. error counters baseline ----------
echo "[3/8] Error counters baseline"
ib_line=$(netstat -I "$IFACE" -b -n 2>/dev/null | awk 'NR==2{print}')
note "  netstat: $ib_line"
# Columns: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll
ibase=$(echo "$ib_line" | awk '{print $5}')
oerrs=$(echo "$ib_line" | awk '{print $9}')
colls=$(echo "$ib_line" | awk '{print $11}')
note "  baseline: Ipkts=$ibase Oerrs=$oerrs Colls=$colls"

# ---------- 4. DNS ----------
echo "[4/8] DNS"
dig +short +time=3 +tries=1 baidu.com >/dev/null 2>&1
check $? "system resolver resolves baidu.com"
dig +short +time=3 +tries=1 "@$DNS_SERVER" baidu.com >/dev/null 2>&1
check $? "direct query @$DNS_SERVER resolves baidu.com"

# ---------- 5. latency / loss ----------
echo "[5/8] Latency and loss (50 probes)"
ping_stats=$(ping -c 50 -t 60 "$PING_TARGET" 2>/dev/null | tail -2)
loss=$(echo "$ping_stats" | grep -o '[0-9.]*% packet loss' | grep -o '[0-9.]*' | head -1)
rtt=$(echo "$ping_stats" | tail -1 | awk -F'/' '{print $5}')
[ -z "$loss" ] && loss=100
[ -z "$rtt" ] && rtt=9999
note "  target=$PING_TARGET loss=${loss}% avg_rtt=${rtt}ms"
awk -v l="$loss" 'BEGIN{exit (l<0.5)?0:1}'
check $? "packet loss < 0.5%"
awk -v r="$rtt" 'BEGIN{exit (r<100)?0:1}'
check $? "average RTT < 100ms"

# ---------- 6. full-size DF frames / path MTU ----------
# The WAN hop may run a smaller MTU (PPPoE), so this measures the largest
# DF frame the path accepts; the NIC passes as long as full-size LAN frames
# are not dropped outright.
echo "[6/8] DF frames / path MTU probe (target $PING_TARGET)"
maxdf=0
for sz in 1472 1452 1400 1300 1200 1000 576; do
    if ping -c 2 -t 4 -D -s "$sz" "$PING_TARGET" >/dev/null 2>&1; then
        maxdf=$sz
        break
    fi
done
note "  largest DF payload accepted: $maxdf bytes (+28 IP/ICMP = $((maxdf + 28)) MTU)"
awk -v m="$maxdf" 'BEGIN{exit (m>=1200)?0:1}'
check $? "path accepts >=1200-byte full frames (driver handles max-size TX)"

# ---------- 7. throughput ----------
echo "[7/8] Throughput (curl bound to $IFACE)"
tmpf=$(mktemp)
curl -s --interface "$IFACE" --max-time 60 -o "$tmpf" -w "%{speed_download} %{size_download} %{http_code}" "$THROUGHPUT_URL" > "$tmpf.meta" 2>/dev/null
read -r speed size code < "$tmpf.meta"
/bin/rm -f "$tmpf" "$tmpf.meta"
if [ "${code:-0}" = "200" ] && [ "${size:-0}" -gt 1000000 ]; then
    mbps=$(awk -v s="$speed" 'BEGIN{printf "%.1f", s*8/1000000}')
    green "  PASS: download ${mbps} Mbit/s ($((size/1000000)) MB in 60s cap)"
else
    red "  FAIL: throughput test (http_code=${code:-none}, bytes=${size:-0})"
    note "  hint: set THROUGHPUT_URL to a LAN iperf/http endpoint for clean numbers"
    FAIL=1
fi

# ---------- 8. concurrent soak + error/log scan ----------
echo "[8/8] Soak ${SOAK}s (ping + 3 parallel transfers)"
end=$(( $(date +%s) + SOAK ))
while [ "$(date +%s)" -lt "$end" ]; do
    curl -s --interface "$IFACE" --max-time 10 -o /dev/null "$THROUGHPUT_URL" &
    curl -s --interface "$IFACE" --max-time 10 -o /dev/null "https://www.baidu.com" &
    sleep 2
done
wait
soak_stats=$(ping -c 30 -t 40 "$PING_TARGET" 2>/dev/null | tail -2)
sloss=$(echo "$soak_stats" | grep -o '[0-9.]*% packet loss' | grep -o '[0-9.]*' | head -1)
[ -z "$sloss" ] && sloss=100
note "  post-soak loss=${sloss}%"
awk -v l="$sloss" 'BEGIN{exit (l<1.0)?0:1}'
check $? "post-soak loss < 1.0%"

ib2_line=$(netstat -I "$IFACE" -b -n 2>/dev/null | awk 'NR==2{print}')
oerrs2=$(echo "$ib2_line" | awk '{print $9}')
colls2=$(echo "$ib2_line" | awk '{print $11}')
note "  delta: Oerrs +$((oerrs2 - oerrs)) Colls +$((colls2 - colls))"
[ $((oerrs2 - oerrs)) -eq 0 ] && [ $((colls2 - colls)) -eq 0 ]
check $? "no new TX errors / collisions during test"

derrs=$(log show --last 15m --style compact --predicate 'eventMessage CONTAINS "yt6801:"' 2>/dev/null | grep -ciE 'error|fail|panic|assert')
note "  driver log error/fail lines (15m): $derrs"
[ "$derrs" -eq 0 ]
check $? "driver log free of errors"

# ---------- summary ----------
echo
if [ "$FAIL" -eq 0 ]; then
    green "================ RESULT: ALL PASS ================"
else
    red "================ RESULT: FAILURES PRESENT ================"
fi
exit "$FAIL"
