#!/usr/bin/env bash
# Local Kubik stack without any OpenAI key:
#   mock OpenAI-compatible server (:18800) + OpenClaw gateway, profile "kubik" (:18789) with the linked
#   openclaw-kubik plugin: device LAN TLS listener kubik://127.0.0.1:18790 (+ UDP discovery on :18790) and the
#   Gateway route ws://127.0.0.1:18789/kubik/v1.
# Idempotent. Logs: tools/.logs/{mock,gateway}.log. Stop with tools/dev-down.sh.
#   KUBIK_SKIP_CONFIG=1 tools/dev-up.sh               # do not touch the profile config
#   MOCK_ARGS="--stt-start 5" tools/dev-up.sh         # extra mock args (first mock transcript = phrase #5)
#   KUBIK_GATEWAY_PORT=19889 KUBIK_DEVICE_PORT=19890 MOCK_OPENAI_PORT=19800 tools/dev-up.sh
#   KUBIK_LOG_DIR=/tmp/kubik-test-logs tools/dev-up.sh # isolate logs and pid files
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$TOOLS")"
PLUGIN="$ROOT/openclaw-kubik"
LOGS="${KUBIK_LOG_DIR:-$TOOLS/.logs}"
MOCK_PORT="${MOCK_OPENAI_PORT:-18800}"
GATEWAY_PORT="${KUBIK_GATEWAY_PORT:-18789}"
DEVICE_PORT="${KUBIK_DEVICE_PORT:-18790}"
mkdir -p "$LOGS"
oc() { (cd "$PLUGIN" && npx --no-install openclaw --profile kubik "$@"); }
listening() { lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }
wait_port() { for _ in $(seq 1 "${3:-120}"); do listening "$1" && return 0; sleep 0.5; done; echo "timeout waiting for $2 on :$1" >&2; return 1; }

[ -d "$PLUGIN/node_modules/openclaw" ] || (cd "$PLUGIN" && npm install --ignore-scripts)
[ -d "$TOOLS/node_modules/ws" ] || (cd "$TOOLS" && npm install --ignore-scripts)

# 1. Dev secrets (gitignored). Never printed.
if [ ! -f "$TOOLS/dev-secrets.json" ]; then
  (umask 077 && node -e '
    const { randomBytes } = require("node:crypto");
    const token = () => randomBytes(24).toString("base64url");
    process.stdout.write(JSON.stringify({ deviceId: "kubik-b6c634", gatewayToken: token(),
      mockApiKey: "sk-mock-local-not-secret", url: "kubik://127.0.0.1:18790" }, null, 2) + "\n");' > "$TOOLS/dev-secrets.json")
  echo "created tools/dev-secrets.json"
fi
# Protocol v5: the device port speaks TLS; older secrets pointed at the plaintext listener.
node -e 'const fs = require("node:fs"); const f = process.argv[1]; const s = JSON.parse(fs.readFileSync(f, "utf8"));
  if (/^ws:\/\/127\.0\.0\.1:18790\//.test(s.url ?? "")) { s.url = "kubik://127.0.0.1:18790"; fs.writeFileSync(f, JSON.stringify(s, null, 2) + "\n"); }' "$TOOLS/dev-secrets.json"

# 2. Mock OpenAI.
if listening "$MOCK_PORT"; then echo "mock-openai: already listening on :$MOCK_PORT"
else
  # shellcheck disable=SC2086 # MOCK_ARGS is a word list, e.g. "--stt-start 5 --tts tone"
  nohup node "$TOOLS/mock-openai/server.mjs" --port "$MOCK_PORT" ${MOCK_ARGS:-} </dev/null >>"$LOGS/mock.log" 2>&1 &
  echo $! >"$LOGS/mock.pid"
  wait_port "$MOCK_PORT" mock-openai 20
  echo "mock-openai: started (pid $(cat "$LOGS/mock.pid"), log $LOGS/mock.log)"
fi

# 3. Profile: linked plugin + config.
if [ -z "${KUBIK_SKIP_CONFIG:-}" ]; then
  if ! oc plugins list --json 2>/dev/null | KUBIK_PLUGIN="$PLUGIN" node -e '
    let input = "";
    process.stdin.on("data", (chunk) => { input += chunk; });
    process.stdin.on("end", () => {
      try {
        const list = JSON.parse(input);
        const path = require("node:path");
        const manifest = require(path.join(process.env.KUBIK_PLUGIN, "package.json"));
        const entry = path.resolve(process.env.KUBIK_PLUGIN, manifest.openclaw.extensions[0]);
        process.exitCode = list.plugins.some((p) => p.id === "kubik" && p.status === "loaded" &&
          p.enabled && p.source === entry) ? 0 : 1;
      } catch { process.exitCode = 1; }
    });'; then
    oc plugins install --link --force --accept-capabilities "$PLUGIN" >>"$LOGS/setup.log" 2>&1
    echo "profile kubik: plugin linked"
  fi
  node "$TOOLS/kubik-profile-patch.mjs" | oc config patch --stdin >>"$LOGS/setup.log" 2>&1
  oc config validate >>"$LOGS/setup.log" 2>&1
  echo "profile kubik: config applied"
fi

# 4. Gateway.
if listening "$GATEWAY_PORT"; then echo "gateway: already listening on :$GATEWAY_PORT"
else
  # Same binary `npx openclaw` resolves to in the plugin checkout (the pinned local install), started without a
  # wrapper so the pid file names the gateway process itself.
  cd "$PLUGIN"
  nohup "$PLUGIN/node_modules/.bin/openclaw" --profile kubik gateway run --port "$GATEWAY_PORT" </dev/null >>"$LOGS/gateway.log" 2>&1 &
  echo $! >"$LOGS/gateway.pid"
  cd "$TOOLS"
  wait_port "$GATEWAY_PORT" gateway 240
  echo "gateway: started (pid $(cat "$LOGS/gateway.pid"), log $LOGS/gateway.log)"
fi
wait_port "$DEVICE_PORT" "kubik device server" 120
echo "kubik: device LAN listener kubik://127.0.0.1:$DEVICE_PORT is up (Gateway route ws://127.0.0.1:$GATEWAY_PORT/kubik/v1)"

# 5. Real device over USB (if one is plugged in): the device authenticates with its own key.
if [ -z "${KUBIK_NO_BRIDGE:-}" ] && ls /dev/cu.usbmodem* >/dev/null 2>&1; then
  BR="$TOOLS/usb-bridge"
  [ -d "$BR/node_modules" ] || (cd "$BR" && npm install --silent >>"$LOGS/setup.log" 2>&1)
  if listening 18791; then echo "usb-bridge: already running"
  else
    nohup node "$BR/bridge.mjs" --dump "$TOOLS/.out/mic" </dev/null >>"$LOGS/bridge.log" 2>&1 &
    echo $! >"$LOGS/bridge.pid"
    wait_port 18791 usb-bridge 20
    echo "usb-bridge: started (pid $(cat "$LOGS/bridge.pid"), log tools/.logs/bridge.log) — hold KEY and talk"
  fi
else
  echo "try:   node tools/fake-device.mjs --turns 3 --approve-local   (no Kubik on USB)"
fi
