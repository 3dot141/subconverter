#!/usr/bin/env bash
# Golden integration test for the chain proxy override feature.
# Requires a built binary at build/subconverter (or pass the path as $1) and curl + python3.
# Starts a local subconverter instance, requests target=clash and target=quanx with a chain
# external config, and asserts the chained-proxy artifacts are present in each output.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${1:-$ROOT/build/subconverter}"

if [ ! -x "$BIN" ]; then
    echo "SKIP: binary not found/executable at $BIN (build the project first)"
    exit 2
fi

NODES=$(python3 -c "import urllib.parse;print(urllib.parse.quote(open('$HERE/nodes.txt').read().strip().replace(chr(10),'|')))")
CFG=$(python3 -c "import urllib.parse;print(urllib.parse.quote('$HERE/external_chain.ini'))")

cd "$ROOT"
"$BIN" >/tmp/subconv_chain.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

BASE="http://127.0.0.1:25500/sub"
CLASH=$(curl -s "$BASE?target=clash&url=$NODES&config=$CFG")
QUANX=$(curl -s "$BASE?target=quanx&url=$NODES&config=$CFG")

fail=0
check() { if echo "$2" | grep -q "$3"; then echo "ok: $1"; else echo "FAIL: $1 (missing: $3)"; fail=1; fi; }

check "clash dialer-proxy"     "$CLASH" "dialer-proxy: JP-Chain-front"
check "clash landing node"     "$CLASH" "my-jp-vps"
check "clash front helper"     "$CLASH" "JP-Chain-front"
# chain-name group: rules target the chain name as a policy, so a group named after the chain must exist
# (regression guard for "proxy [JP-Chain] not found" when the retired relay group left no replacement).
check "clash chain-name group" "$CLASH" "name: JP-Chain,"
if echo "$CLASH" | grep -q "type: relay"; then echo "FAIL: clash relay group not retired"; fail=1; else echo "ok: clash no relay group"; fi
check "quanx backhaul ip-cidr" "$QUANX" "ip-cidr, 203.0.113.9/32"
check "quanx via-interface"    "$QUANX" "via-interface=%TUN%"

# BF1 — anytls QuanX line
check "quanx anytls line"      "$QUANX" "anytls = 198.51.100.50:4048"
check "quanx anytls over-tls"  "$QUANX" "over-tls=true"
check "quanx anytls tls-host"  "$QUANX" "tls-host=cdn.example.com"

# BF2+BF3+BF4 — multi-node landing via []group ref
check "clash LD-US dialer"     "$CLASH" "dialer-proxy: US-Chain-front"
check "quanx backhaul vps1"    "$QUANX" "ip-cidr, 192.0.2.11/32"
check "quanx backhaul vps2"    "$QUANX" "ip-cidr, 192.0.2.12/32"
check "quanx landing group"    "$QUANX" "usa-landing, via-interface=%TUN%"

if [ $fail -eq 0 ]; then echo "PASS"; else echo "----- /tmp/subconv_chain.log -----"; tail -20 /tmp/subconv_chain.log; exit 1; fi
