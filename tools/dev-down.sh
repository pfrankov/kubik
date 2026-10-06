#!/usr/bin/env bash
# Stops what tools/dev-up.sh started (gateway of profile "kubik" and the mock). Only processes whose command
# line identifies them as ours are touched; the profile config and secrets stay in place.
set -uo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
LOGS="${KUBIK_LOG_DIR:-$TOOLS/.logs}"
MOCK_PORT="${MOCK_OPENAI_PORT:-18800}"
GATEWAY_PORT="${KUBIK_GATEWAY_PORT:-18789}"
DEVICE_PORT="${KUBIK_DEVICE_PORT:-18790}"
# A process is ours if it or one of its parents was started for profile "kubik" (the gateway renames itself to
# "openclaw-gateway", like any other profile's gateway), or it is the mock.
ours() {
  local pid="$1"
  for _ in 1 2 3 4; do
    [ -z "$pid" ] || [ "$pid" -le 1 ] && return 1
    ps -o command= -p "$pid" 2>/dev/null | grep -Eq -- "--profile kubik|mock-openai/server\.mjs" && return 0
    pid="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d " ")"
  done
  return 1
}
stop_port() {
  local port="$1" name="$2" pid killed=""
  for pid in $(lsof -nP -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null | sort -u); do
    if ours "$pid" || grep -qx "$pid" "$LOGS"/*.pid 2>/dev/null; then
      kill "$pid" 2>/dev/null && killed="$killed $pid"
    else
      echo "$name: :$port is held by pid $pid which is not ours ($(ps -o command= -p "$pid" | cut -c1-80)); leaving it"
    fi
  done
  [ -z "$killed" ] && { echo "$name: not running"; return; }
  for _ in $(seq 1 20); do
    local alive=""; for pid in $killed; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ -z "$alive" ] && break; sleep 0.5
  done
  for pid in $killed; do kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null; done
  echo "$name: stopped (pid$killed)"
}
if [ -f "$LOGS/bridge.pid" ]; then
  pid=$(cat "$LOGS/bridge.pid")
  if ps -o command= -p "$pid" 2>/dev/null | grep -q "usb-bridge/bridge.mjs"; then kill "$pid" && echo "usb-bridge: stopped (pid $pid)"; fi
  rm -f "$LOGS/bridge.pid"
fi
stop_port "$GATEWAY_PORT" gateway
stop_port "$DEVICE_PORT" "kubik device server"
stop_port "$MOCK_PORT" mock-openai
rm -f "$LOGS/gateway.pid" "$LOGS/mock.pid"
