# openclaw-kubik

An OpenClaw channel for **Kubik**, a Wi-Fi voice companion built on the Waveshare ESP32-C6 AMOLED board. Your OpenClaw agent keeps its context and tools; Kubik provides the screen, microphone, speaker and Tess character.

## Install

Run these commands on the **computer or server running OpenClaw**, not on your phone:

```sh
openclaw plugins install openclaw-kubik --force --accept-capabilities
openclaw channels add --channel kubik --enable
openclaw channels status --probe
```

Requires OpenClaw **2026.9.7 or newer in the 2026 series**, Node.js **24.16+ in 24.x or 26.1+**, and Kubik firmware **0.6.2**. Review the plugin's capabilities before accepting them. A running Gateway applies the installation through its managed update; allow the device to reconnect. If the Gateway is stopped, use `openclaw gateway start` for an installed service or `openclaw gateway run` in a terminal.

To update the plugin, repeat the installation command. Preserve the OpenClaw state directory and device settings. Do not reset Kubik for an ordinary update.

## Connect and approve

1. Scan Kubik's first QR to join its setup Wi-Fi. Scan the second QR to open the setup page.
2. Choose your 2.4 GHz Wi-Fi. On the same trusted network, leave the server address blank for discovery. Otherwise enter a reachable address as described below.
3. Run the **complete command shown on Kubik** on your OpenClaw computer:

```sh
openclaw pairing approve kubik CODE
```

Replace `CODE` with the device's current code. You can also approve it in **OpenClaw Web UI → Channels → Pairing requests**. Then follow Kubik's short guide and press **KEY** to talk. A full factory reset creates a new device key and requires approval again.

Each device has a separate agent session (`kubik:<deviceId>`). Choosing an agent model on Kubik changes that session, not your Telegram or other channels.

## Server address

`openclaw channels status --probe` shows the Kubik listener, for example `lan 192.168.1.5:18790 spki:…; route /kubik/v1`.

| Location | Address on Kubik |
| --- | --- |
| Same trusted network | Leave blank, or `kubik://SERVER-IP:18790` |
| Remote server with a public HTTPS endpoint | `wss://YOUR-DOMAIN/kubik/v1` |
| Public server without a domain | `kubik://PUBLIC-IP:18790`, with public listening enabled |

Use an address reachable **from Kubik's Wi-Fi**. `localhost` on a computer and a private VPN address do not give Kubik access unless the network routes to them.

The LAN listener uses TLS and remembers its identity in the OpenClaw state directory (`kubik/lan-tls.json`, mode 0600). Kubik pins it on first successful pairing. Allow TCP and UDP 18790 inside your local network for connection and discovery. If broadcast discovery is blocked, enter the LAN address manually.

For a public IP without a domain:

```sh
openclaw config set channels.kubik.listen.public true
```

Allow inbound TCP 18790 in the server and hosting-provider firewalls, preferably restricted to your home IP. UDP discovery remains local. Keep the Gateway dashboard port private.

For a public domain, forward **only `/kubik/v1`** to the Gateway port (normally 18789), with a valid public CA certificate. Configure the Gateway's trusted proxies for your actual proxy. A Cloudflare Tunnel example is in the [deployment guide](https://github.com/pfrankov/kubik/blob/main/deploy/cloudflared/README.md). Tailscale Funnel exposes the whole Gateway rather than one path; follow OpenClaw's Gateway authentication requirements.

## Voice and models

Provider credentials stay on the OpenClaw computer. The default provider (`openclaw`) reuses OpenClaw's configured speech recognition and synthesis; Kubik does not need a separate OpenAI account for Classic STT/TTS.

- **STT / Classic:** recognition → the existing agent → speech synthesis. Responses are spoken in order as agent blocks become available. Recognition and Speech models are separate.
- **Realtime:** streaming voice input; a pause ends the question. Ready agent response blocks begin synthesis before the entire dispatch finishes.
- **GPT Live:** continuous voice connection, started with KEY or “Hi Tessa” on Tess. Pauses do not stop Live; KEY, a tap, menu, sleep, lost connection or an error ends it. The host subtracts speaker echo, and voice activity can interrupt playback while retaining the agent context.
- **Text only:** unavailable TTS or Speech volume below 20 shows the agent's answer as text.

On Kubik, **Settings → Agent → Voice** selects a mode and its available models. Classic has separate Recognition/Speech catalogs; native modes share one voice model. Choices persist on the host for the account and paired device key. They do not change the agent's text model. An empty catalog can mean the provider supports speech without selectable models.

Realtime/GPT Live require configured **OpenAI Platform API authorization on the Gateway** and supported firmware. A ChatGPT subscription alone does not supply an API key. Configure providers, credentials and voices on the server; the device reports availability rather than installing providers. See the [voice setup guide](https://github.com/pfrankov/kubik/blob/main/docs/buyer/START.html) and [native voice details](https://github.com/pfrankov/kubik/blob/main/docs/native-voice.md).

Classic recognition uses `tools.media.models` with audio capability or OpenClaw's provider detection. Synthesis uses `tts.provider`; `channels.kubik.tts` can override the voice for Kubik:

```sh
openclaw config set channels.kubik.tts '{"providers":{"openai":{"speakerVoice":"ash"}}}' --strict-json
```

The alternative `openai-http` provider uses an OpenAI-compatible STT/TTS endpoint. Its keys, `baseUrl` and voice models are configured on the host. `allowInsecureBaseUrl` is for a local HTTP mock only: it permits sending credentials without TLS.

## Configuration

Configuration lives in `channels.kubik`, with one implicit `default` account. Devices pair automatically; optional `devices` entries set a name or disable a device.

```json
{
  "channels": {
    "kubik": {
      "enabled": true,
      "voice": { "provider": "openclaw", "language": "en" },
      "devices": {
        "kubik-xxxxxx": { "name": "Desk Kubik", "enabled": true }
      }
    }
  }
}
```

Revoke a device with:

```sh
openclaw config set channels.kubik.devices.kubik-xxxxxx.enabled false
```

Replace the sample ID with your device ID. Existing connections and queued notifications recheck authorization. Reapproval does not deliver messages discarded after revocation.

The schema in `src/config.js` is authoritative. Supported voice providers are `openclaw` and `openai-http`; `voice.mode` is `classic`, `realtime` or `live`. Old providers (`openai-realtime`, `openai-live`, `openai-gpt-live`), `voice.model`, `voice.instructions` and `devices.<id>.prefs` are not accepted: remove obsolete settings explicitly before updating. There is no automatic engine substitution.

## Agent activity and notifications

Kubik displays agent thinking, tool use and other activity. Work in the device's own session shows its thinking state; other Gateway activity uses a small status bubble. Background status changes do not wake a dark screen.

Send a notification through the Gateway:

```sh
openclaw message send --channel kubik --target kubik-xxxxxx -m "Your task is ready."
```

Approved offline devices have a durable notification queue: up to eight messages per device, 128 total, expiring after 24 hours. Delivery is removed after the device acknowledges it. A connection loss before acknowledgment can repeat a partially displayed or spoken reply. Explicit user cancellation completes delivery.

Emotion tags such as `[[happy]]`, `[[sad]]` and `[[proud]]` animate Tess alongside speech. Markdown, code, URLs and screen-only content are stripped from synthesis. Agent questions keep the run waiting for the user's next response.

## Security and limits

- Devices authenticate with a fresh challenge and P-256 signature bound to the TLS transport. Device private keys are not transmitted; firmware stores them in unencrypted NVS, so physical flash access can expose them.
- Approval binds both device ID and key. One device has one active connection; a new connection replaces the previous one. Pairing expires after ten minutes. Admission, audio buffers, retries and queues are bounded.
- LAN uses pinned TLS; remote WSS validates a CA certificate. Plain `ws://` is rejected. Preserve the server TLS identity and pair only with your intended server.
- Only conversation-control slash commands are authorized from recognized speech: model, reasoning, fast mode, status, stop and new/reset session. Other recognized text is untrusted input.
- Canceling speech **does not cancel the agent's tools or side effects**. The agent run can finish in the background; its superseded voice blocks are not played.
- First-audio latency also depends on the agent/provider. Some providers do not expose a final reply block until generation finishes. Provider readiness is not proof of paid API access or response speed.
- Live is limited to 30 minutes and a single reply to 120 seconds. Other limits and protocol errors are documented in the [protocol](https://github.com/pfrankov/kubik/blob/main/docs/protocol.md).

## Custom agents and development

The package also exports `openclaw-kubik/agent-sdk` and the `kubik-agent-host` CLI for custom ESM adapters. Hermes has a separate native Gateway platform. The device setup wizard currently offers OpenClaw only. Read the [adapter SDK guide](https://github.com/pfrankov/kubik/blob/main/docs/agent-sdk.md) for extension contracts and a runnable example.

Tests and development scripts are in the source repository, not the installed npm package:

```sh
cd openclaw-kubik
npm ci
npm run check
npm run check:host
node ../tools/test-install.mjs
```

The repository provides a local mock for tests without paid API calls. Full device/firmware acceptance is documented in [acceptance](https://github.com/pfrankov/kubik/blob/main/docs/acceptance.md). The generated manifest explicitly declares no current state migrations in `doctorContract`; schema changes must update that contract together with actual migrations.

## License

Apache-2.0 for Kubik's own code. Included third-party code retains its notices in `licenses/` and `NOTICE`. Firmware assets and the Tessa wake model have separate terms in the source repository; the wake model is not included in this npm package.
