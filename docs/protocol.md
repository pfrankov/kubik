# Kubik device protocol v5 · USB transport v2

Transport between the desk device (ESP32-C6) and an agent host, including the `kubik` OpenClaw channel
plugin. The same frames are carried either over a WebSocket (Wi-Fi) or over the
USB tether (see "USB link" below; the host bridge converts USB frames into the
same WebSocket session, so the plugin sees one protocol).

The device never talks to OpenAI directly and never stores provider keys. It
holds its own ECDSA P-256 private key, generated locally and never sent by the
protocol. In the current self-install build, that key and the Wi-Fi password
reside in unencrypted NVS; someone with physical flash access could extract
them. Devices authenticate with protocol v5 by signing a server challenge bound
to the TLS transport they see; there is no shared device-token authentication
path. v3 and older are not accepted.

## Transports

Two transports carry the same WebSocket protocol at path `/kubik/v1`; the plugin runs both, with no
configuration:

1. **LAN (default).** The plugin creates once a P-256 key and a self-signed X.509 v3 certificate (hand-built
   DER, valid 1950-01-01 … 9999-12-31, no extensions) and keeps it in the OpenClaw state dir as
   `<stateDir>/kubik/lan-tls.json` (JSON with PEM key and certificate, mode 0600, atomic write). An unreadable
   or corrupt file is moved aside as `lan-tls.json.corrupt-<ms>` and a new key is created (pinned devices then
   report "server key changed"). It listens with TLS ≥ 1.2 on TCP port `channels.kubik.listen.port` (default
   18790) on all interfaces (`::` dual stack, `0.0.0.0` without IPv6). A TCP peer that is not private is
   closed before any TLS byte: loopback, 10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 (CGNAT,
   Tailscale), `::1`, fc00::/7, fe80::/10 and the IPv4-mapped forms. Before the WebSocket upgrade (where the
   pre-session limits below start) the listener holds at most 64 TCP connections, 8 per peer address, and bounds the
   TLS handshake and the upgrade request to 10 s each. With `channels.kubik.listen.public: true` (default false;
   a VPS with a public IP and no domain, the device server field `kubik://<public IP>`) the TLS listener serves any
   valid peer address with the same key, pin and limits; UDP discovery still answers private peers only, and the
   status line says `public`. The first contact over the internet is trust on first use: an active attacker on the
   path can be pinned instead of the server, but a relay to the real server fails the v5 bind (`4001`), so the
   real server shows no pending request for the code on the device screen; firewall the host to 18790/tcp. A
   busy port is not fatal: the status shows the error and the Gateway route keeps working.
2. **Gateway route.** The plugin registers the Gateway HTTP route `/kubik/v1` (`auth: "plugin"`: devices prove
   their key in this protocol; exact match) with an upgrade handler, so whatever already exposes the OpenClaw
   Gateway (Tailscale Serve/Funnel via `gateway.tailscale`, Cloudflare Tunnel, a reverse proxy) serves devices
   at `wss://<public-host>/kubik/v1`. The device validates that TLS with its CA bundle (needs SNTP time). A
   plain request gets `426`; an upgrade while the channel is not running gets `503`.

**Multiple accounts.** The channel has one implicit account (`default`). The route and the LAN listener belong
to it; were more accounts ever configured, the route would serve only `default` and each further account's
LAN listener would need its own `listen.port`.

### Discovery (LAN)

* The device broadcasts the ASCII datagram `kubik-discover-v5` to `255.255.255.255:18790` (UDP), up to 3
  tries 1 s apart, on every (re)connect attempt while in LAN discovery mode.
* The plugin's UDP socket on `0.0.0.0:18790` answers only the exact request from private peers (same list as
  above), at most 10 replies per second in total, by unicast: `{"t":"kubik","v":5,"port":18790}` (`port` is
  the TLS listener's port). The device uses the reply's source IP: `wss://<ip>:<port>/kubik/v1`.
* Discovery is best effort: a busy UDP port leaves the listener reachable through `kubik://host`.

### Device server setting (portal field "server")

* empty → LAN mode with discovery.
* `kubik://host[:port]` → LAN mode without discovery (networks that block broadcast); port default 18790.
* `wss://host[:port]/kubik/v1` → CA mode (public certificate, e.g. the Gateway route behind Tailscale or Cloudflare); it signs bind `ca:<host>`, see "Host binding" below.
* `ws://…` → development only; it signs bind `none`, which the plugin never accepts.

### Pinning (LAN mode)

The device pins the SHA-256 of the leaf certificate's SubjectPublicKeyInfo DER (32 bytes in NVS), verified in
its own mbedTLS verify callback (no CA chain, no time check). With no pin stored it accepts the leaf (TOFU),
remembers the hash for this connection and persists it only after `welcome`. With a pin stored the leaf must
match or the handshake fails; the device shows "server key changed" and never re-pins by itself. An explicit
save of the connection settings (portal or USB `set`) clears the pin.

### Native Hermes Gateway platform

[Hermes setup](hermes.md) uses the same v5 signed challenge and SPKI pinning on
`kubik://HOST:18793`, path `/kubik/v1`. Its listener lives inside Hermes Gateway,
not a separate companion process. It does not implement UDP discovery or the
OpenClaw Gateway route. Approve the device's code with `python
HERMES_HOME/plugins/kubik/pair.py CODE` as its service user. Its private approval
store binds the same device ID and key fingerprint; replacing a device key
requires another approval. Classic voice uses Hermes's own STT/TTS pipeline;
Realtime/Live and device model selection are not provided by this platform.

## WebSocket frames

* Text frames: one JSON object, UTF-8, max 4 KiB.
* Binary frames: audio. Byte 0 = kind, byte 1 = sequence tag, rest = audio.

Audio is **mono, 24 000 Hz** in both directions: IMA ADPCM on the wire; adapters receive/produce PCM s16le.
The current channel requires v5 host-bound or SPKI-bound signatures. Provider adapters may resample their output before sending it;
the OpenClaw adapter normalizes TTS to this format.

| Byte 0 | Direction | Byte 1 | Payload |
|---|---|---|---|
| `0x04` | device → server | `turn` (u8) | Classic/Realtime microphone IMA: 3-byte header + up to 480 bytes (40 ms) |
| `0x05` | device → server | `turn` (u8) | Live: MIC1 IMA + synchronized codec loopback IMA; exactly 968 bytes total |
| `0x03` | server → device | `gen` (u8) | speech IMA ADPCM: `pred` (int16 LE), `index` (u8), then 4-bit codes, low nibble first |

Microphone packets decode independently from their header (`index` 0..88), low nibble first;
the complete binary frame is 6..485 bytes. Firmware emits 960 samples per 40 ms frame,
keeps encoder state within a turn/session and resets it at their boundaries. Local VAD
uses the original PCM. The host restores PCM before passing it to any agent adapter.
Protocol v5 requires matching host and firmware; v4 hello and raw kind `0x01` are rejected.
Device keys, Wi-Fi and pairing remain stored when both components are updated.

IMA ADPCM is a quarter of the PCM bytes (12 KB/s). The device allocates a complete 900 ms / 10.8 KB
speech ring or refuses playback. One gen is one
continuous ADPCM sequence: `pred`/`index` is the encoder state before the frame, and the device starts decoding
from the header of the gen's first frame. The step table and nibble order are the standard IMA ones
(`firmware/main/ima_adpcm.c`, `openclaw-kubik/src/protocol.js`; both check the same test vector).

`turn` identifies the push-to-talk utterance; `gen` identifies one spoken
reply. The device drops speech frames whose `gen` is not the active one, so
audio arriving after a cancel can never play.

## Device → server JSON

```json
{"t":"hello","v":5,"device":"kubik-b6c634","key":"<base64 P-256 public key>","fw":"0.6.2","name":"Kubik"}
{"t":"auth","sig":"<base64 DER ECDSA signature>"}
{"t":"ptt","on":true,"turn":7}
{"t":"ptt","on":false,"turn":7,"ms":2380}
{"t":"cancel","gen":3}
{"t":"progress","gen":3,"ms":1750}
{"t":"played","gen":3,"ms":4120}
{"t":"poke","kind":"tap"}
{"t":"ping","ts":123456}
```

* `hello` must be the first frame (see "Authentication and pairing" below);
  `auth` answers the server's `challenge`. `device` is stable (derived from
  the MAC). `name` (optional) is 1–32 characters with no control or format characters;
  `fw` is 1–32 printable ASCII characters without spaces; anything else closes with `4002`.
  Firmware also includes `server`, its configured server setting (empty, `kubik://…`, `wss://…`
  or `ws://…`). The USB bridge uses it as its default destination and removes that transport hint
  before relaying hello. Direct WebSocket servers ignore the extra field.
Automatic recording uses `{"t":"ptt","on":true,"turn":5,"automatic":true}`.
An empty/interrupted automatic turn uses `{"t":"cancel","turn":5}` on that connection.

* `ptt on` starts a new utterance. Microphone frames follow with the same
  `turn`. `ptt off` closes it; `ms` is the held duration. The server treats
  `ptt off` as the explicit end of the phrase (no server VAD).
  TESS voice wake sends `automatic:true` on PTT-on. The host rejects it with
  `busy` before interrupting anything if a turn, output, notification receipt,
  own activity or model change is already active. Manual KEY can interrupt speech; a pending model change still rejects it as busy.
* `cancel` abandons the current recording/transcription and interrupts pending
  or playing output. An empty automatic turn uses it without `ptt off`, so STT
  and the agent are not invoked. It does not undo an agent action that already ran.
  `cancel` with `turn` discards only that matching recording/transcription and
  leaves speech/notifications intact. An unmatched turn is ignored; `turn` and
  `gen` are mutually exclusive. Automatic capture uses this scoped form and its
  originating connection. A cancelled transcription may settle but cannot dispatch.
* `progress` (when `welcome` carries `"progress":true`): every 100 ms while `gen` plays, how many
  ms of it the device has consumed from the decoded speech ring; one already mixed 20 ms frame may still be in the codec write path. The server paces speech by it: it keeps at most the device buffer
  minus a margin unplayed (sent − played), counting audio still in flight as buffered, so a late start, a
  network stall or a burst is bounded. An incomplete ring allocation refuses playback; incomplete playback cannot acknowledge delivery.
* `played` reports that the decoded speech ring for `gen` has drained. `ms` is required; delivery requires a matching generation and `ms` at least `max(1, floor(total sent PCM duration in ms))`. Empty, refused or partial playback does not consume a notification. Firmware counts decoded ring samples; one already mixed 20 ms frame may still be in the codec write path. The six TX DMA descriptors can retain up to 60 ms after a codec write. This receipt does not establish physical speaker completion. Before codec doze, firmware keeps writing silence for an 80 ms drain guard derived from DMA capacity plus one 20 ms mixed frame.
* `poke` is an optional interaction hint (`tap`, `pet`, `shake`, `pickup`) — the
  server may ignore it. Any other lowercase kind is accepted as `other`: a gesture
  from newer firmware never costs the connection (and the turn in progress).
* Removed `prefs` frames are rejected as unknown types (close 4002). `agent_model` requires
  a target: `agent`, `mode`, `voice`, `stt`, or `tts`.
  Providers and credentials are configured on the host; voice mode is selected per device. See [Agent controls](#agent-controls-and-speech-capabilities-062).

## Server → device JSON

```json
{"t":"challenge","nonce":"<base64 of 32 random bytes>"}
{"t":"pair","code":"ABCD2345"}
{"t":"welcome","session":"s-1","progress":true,"volume":70}
{"t":"state","s":"thinking"}
{"t":"emotion","e":"happy","ms":4000}
{"t":"speak","gen":3,"kind":"reply"}
{"t":"speak_end","gen":3}
{"t":"text","text":"Код: ABCD-1234","kind":"reply"}
{"t":"activity","own":"web","other":""}
{"t":"cron","running":1,"next":570}
{"t":"error","code":"stt_empty"}
{"t":"set","volume":60,"brightness":180}
{"t":"pong","ts":123456}
```

* Plush enables microphones only for KEY capture. Tess also runs a local Tessa
  detector on an awake or dimmed idle home screen when connected with STT available. Its
  user phrase is Hi Tessa; the model targets Tessa and does not enforce the Hi prefix. Detection starts
  the same PTT wire turn; local VAD sends PTT-off after a pause. Passive PCM is
  never uploaded. Setup/menu/guide/pairing/own-agent work/active replies/sleep/TLS disable the detector.
  There is no direction tracking. Tess reacts to the real playback envelope.
* `state.s` ∈ `idle`, `listening`, `transcribing`, `thinking`, `speaking`.
  The device owns the *listening* indicator locally (it is derived from the
  physical key and the capture gate, never from the server).
* `emotion.e` ∈ `neutral happy joy love sad angry surprised confused sleepy
  thinking wink shy proud`. `ms` is optional (default: until next emotion).
* `speak.kind` ∈ `reply` (answer to the user), `notify` (proactive message
  from the agent: reminder, cron, message tool). `notify` makes the device
  play a chime and an attention animation before the speech.
* `speak_end` means the server has sent all audio for `gen`; the device still
  plays what is buffered and then sends `played`.
* `text` (firmware ≥ 0.3.0) shows a text card over the face: UTF-8, at most 1000
  bytes, `\n` breaks lines; `kind` ∈ `reply`, `notify` (a `notify` card with no
  speech plays the chime). A later `text` of the same reply repeats the whole card
  (the device keeps its page when the new text extends the shown one). The server
  sends it for `[[show]] … [[/show]]` blocks in agent replies (shown, never spoken),
  for replies it could not speak (TTS failed or unavailable — then no
  `voice_failed` error is sent), and for every reply when
  `channels.kubik.text = "always"`. The device word-wraps and pages it; a tap
  turns the page or closes the card, a new push-to-talk closes it.
* `activity` (firmware ≥ 0.3.0; older firmware ignores it) says what OpenClaw is busy
  with right now, in the categories of its status reactions: `thinking` (🧠),
  `tool` (🛠️), `coding` (💻), `web` (🌐), `deploy` (🛫), `build` (🏗️), `concierge`
  (browser automation), `compacting` (🗜️), `stall` (⏳ no progress for 30 s), or `""`
  (nothing). `own` is a run in this device's own agent session, or a heartbeat that
  will report to it (its typing signal); the device shows it as *thinking* with that
  activity in the status bubble. `other` is any other run in the Gateway (another
  channel, an isolated cron job, a subagent); the device shows only a status bubble
  while nothing else needs it. Sent on change and again after a reconnect (never an
  empty one right after `welcome`); the device clears both when the link drops. An
  unknown category counts as `tool`.
* `cron` (firmware ≥ 0.3.0) is OpenClaw's cron state for the corner indicator:
  `running` = jobs running now (script jobs too, they never show up as `activity`),
  `next` = seconds until the nearest enabled one-shot (`at`) job, a reminder, or
  `-1`. Fed by the plugin's `cron_changed`/`cron_reconciled` hooks and the
  scheduler's list; sent when the running count or the due time changes, and after
  `welcome` unless both are empty. The device counts down by itself. This passive
  snapshot never wakes the screen or resets idle time; reminders use actual notifications.
* `error.code` ∈ `stt_empty` (nothing recognised), `stt_failed`,
  `agent_failed`, `voice_failed`, `busy`, `unauthorized`.

## Authentication and pairing (v5)

```
device                                   server
hello {v:5, device, key}      ──────▶    validate key
                              ◀──────    challenge {nonce}
auth {sig}                    ──────▶    verify signature
                              ◀──────    welcome                  (key approved)
                              ◀──────    pair {code} … welcome    (new key: after approval, same socket)
```

1. `hello.key` is the standard, padded base64 of the 65-byte uncompressed SEC1
   P-256 public key `0x04 || X || Y`. The server rejects anything that is not a
   valid point on the curve (close `4002`). `device` must be a valid device id
   (`[a-z0-9][a-z0-9_-]{0,63}` after lowercasing).
2. The server answers `challenge` with a fresh 32-byte nonce (base64).
3. The device signs, with ECDSA P-256 / SHA-256, the UTF-8 bytes of

   ```
   kubik-auth-v5\n<nonce>\n<device>\n<key>\n<bind>
   ```

   where `<nonce>`, `<device>` and `<key>` are the exact strings sent on the
   wire (`\n` is a single LF byte, no trailing newline) and `<bind>` is the
   transport binding the device observed: the lowercase hex SHA-256 of the
   server certificate's SPKI (LAN mode), `ca:<host>` (`wss://` validated with
   the CA bundle; `<host>` is the host it connected to, see "Host binding") or
   `none` (`ws://`). It sends
   `{"t":"auth","sig":"<base64 DER signature>"}`. The server verifies against
   the binding of the transport that accepted the socket: the LAN listener
   expects the hex SHA-256 of its own SPKI, the Gateway route expects
   `ca:<host the request arrived on>`; `none` is never accepted. A relaying TLS
   man-in-the-middle therefore makes the device sign the relay's key, and a
   server that is only reachable under another public name gets a signature
   for that name: both fail. A bad signature or a
   binding mismatch gets `{"t":"error","code":"unauthorized"}` and close `4001`. Any other frame
   after the challenge closes with `4002`. `hello` must arrive within 3 s of the connection
   opening and `auth` within 3 s of the challenge.
   **Host binding (Gateway route, firmware 0.6.1 and newer).** A CA certificate only proves that some server has a
   public certificate. To keep a device that was pointed at the wrong server from having its handshake relayed to
   the real one, the device signs `ca:<host>`, where `<host>` is the host of its `wss://host[:port]/path` server
   setting, normalised exactly like this: ASCII lowercase; no port; an IPv6 literal without the brackets;
   no trailing dot. The bind uses only `[0-9a-z.:-]` and is at most 127 characters (`<host>` at most 123). The USB bridge
   reports the same value in frame `0x23`. LAN SPKI binds and `none` are unchanged.

   The server derives `<host>` from the upgrade request with the same normalisation, then compares:

   * `Host` is authoritative. It must be present exactly once and be a plain `host[:port]` (or `[ipv6][:port]`) of
     letters, digits, `.` and `-`, with labels of at most 63 characters (no comma list, whitespace, `_`, `@`, `/`,
     non-ASCII, empty label, or empty or non-numeric port); anything else gets `400` before any handshake and a log
     line `Host header is missing, repeated or not a plain host name`.
   * `X-Forwarded-Host` counts only when the socket's direct peer is inside `gateway.trustedProxies` (OpenClaw's own
     matcher, single addresses and CIDR ranges; the same list the Gateway uses for `X-Forwarded-For`) **and** `Host`
     looks like what a proxy leaves when it talks to its upstream by address: an IP address or a name without a dot
     (`127.0.0.1:18789`, `[::1]`, `openclaw:18789`). A public name in `Host` is never overridden: a client can
     send its own `X-Forwarded-Host` through a proxy that does not overwrite it. Of a comma list the last value
     is used, the one the trusted proxy itself appended. An unusable value gets `400`.
   * The server trusts these headers only as far as its front does: `Host` is chosen by whoever sends the request,
     so this binding stops a relay only where the public name reaches the Gateway through Tailscale, Cloudflare or
     another proxy that routes by name. Do not expose the Gateway port itself to the internet for this route.

   **Operator rules.** A proxy in front of the Gateway must keep the public name in `Host`, or the operator must add
   the proxy's address to `gateway.trustedProxies` and make the proxy send the public name in `X-Forwarded-Host`
   (overwriting any client value). Otherwise every device is refused: the Gateway log has
   `kubik: rejected device "kubik-…" from <ip> via gateway (bad signature or transport binding mismatch: another
   server key or host, or a relay in between; this request arrived on host "127.0.0.1" (Host header))`, and the
   device sees `unauthorized` (close `4001`). The quoted host is the one the server compared, so it shows what the
   proxy passes on. Cloudflare Tunnel keeps the public name in `Host` unless the application sets `httpHostHeader`;
   behind Tailscale Serve/Funnel check the first connection's log line.

   Hostless `ca` signatures are refused for every firmware version. Update the device to use `ca:<host>`.
4. The device identity is `<deviceId>:<fp>`, where `fp` is the first 32 hex
   characters of sha256 of the raw 65-byte key. It binds the claimed device id
   to the key: an approval for one id does not authorize the same key under
   another id, nor another key under the same id.
5. If the identity is in OpenClaw's pairing allow list for the `kubik` channel,
   the server continues as for any authenticated device (`welcome`, `4003`
   replacement, …).
6. Otherwise the server creates (or refreshes) a pairing request and sends
   `{"t":"pair","code":"ABCD2345"}`. The device shows the code; the owner
   approves it on the Gateway host:

   ```sh
   openclaw pairing list kubik
   openclaw pairing approve kubik ABCD2345
   ```

   The socket stays open without a session; the device may only send `ping`
   (answered with `pong`), anything else closes with `4002`. The server checks
   the allow list every 2 s and sends `welcome` on the same socket as soon as
   the key is approved. After 10 minutes without approval it closes with
   `4004`; the device reconnects and gets the same code while the request lives
   (OpenClaw keeps pending requests for 1 hour).
7. OpenClaw keeps at most 3 pending requests per channel. When it cannot create
   another one the server sends `{"t":"pair","code":""}` and closes with `4005`.

The server stores only the `deviceId:fingerprint` allow entry (in OpenClaw's
pairing store); there are no shared secrets in `openclaw.json`. A key is
revoked by `channels.kubik.devices.<deviceId>.enabled: false` (close `4001`,
even for an approved key), or by removing its entry from the allow list.

Limits: at most 16 simultaneous pre-session sockets per server and 4 per
client address. A socket holds this slot while the server reads the pairing
allow list, including after a valid signature; moving to an approved session or
pending-pairing state releases it. Separately, 4 unapproved devices may wait
for pairing globally and 2 per client address (beyond that: close `4005`
without creating a request). A failed signature holds its pre-session slot for
300 ms before close; there is no persistent identity or address lockout that a
stranger could use to disable an approved device. On the LAN listener the client address is the TCP peer; proxy headers are never read.
On the Gateway route it is the address the Gateway itself resolved: behind Cloudflare Tunnel or another reverse
proxy, add the proxy's address to `gateway.trustedProxies`, otherwise every remote device shares the proxy's quota
(the same setting lets `X-Forwarded-Host` name the host, see "Host binding").

## Flow of one push-to-talk turn

```
device                   kubik plugin / voice engine       OpenClaw agent
ptt on          ──────▶  beginTurn()
PCM frames      ──────▶  append()
ptt off         ──────▶  commit() → STT
              ◀───────  state transcribing
                         transcript ─────────────────────▶ route/session
              ◀───────  state thinking
                         ◀─────────────────────────────── reply blocks
                         parse emotions → speak() → TTS
              ◀───────  emotion / speak / paced PCM
              ◀───────  speak_end
played          ──────▶ buffer drained
              ◀───────  state idle
```

The currently deployed `voice.provider=openclaw` calls the Gateway's own STT/TTS
APIs. `openai-http` (OpenAI-compatible `/audio` API) is the other implemented adapter. The speech-to-speech
engines (`openai-realtime`, `openai-live`, `openai-gpt-live`) were removed; configurations that still
name them are rejected and must be migrated explicitly.

Pacing: the server keeps at most 800 ms of sent-but-unconfirmed speech, including
network transit, against the required 900 ms device buffer. Only device consumption
releases credit. Wall-clock estimates schedule emotion markers and delivery deadlines;
they do not delay a refill after fresh progress or grant credit without it.

## USB transport v2 (tether and configuration)

The host writes at most 512 bytes per chunk, waits for the serial drain and
leaves a 5 ms gap before the next chunk. USB Serial/JTAG ignores UART baud rate;
ESP-IDF's 4096-byte RX ring otherwise silently drops an oversized WebSocket burst.
Framing/order are unchanged. The host queue is capped at 32 KiB, each write/drain
at 1 s; failure closes the route and port, discarding queued bytes. Closing a port
cancels pending chunks and timers before attaching a replacement.

The USB Serial/JTAG port carries a framed byte stream. Anything outside frames
(bootloader output, panics) is plain text and is printed by the host tool.

```
0xA5 0x5A  type(u8)  len(u16 LE)  payload[len]  crc8(payload)
```

| type | direction | payload |
|---|---|---|
| `0x01` | both | `epoch(u32 LE)` followed by JSON text |
| `0x02` | both | `epoch(u32 LE)` followed by WebSocket binary audio |
| `0x10` | device → host | log line |
| `0x20` | host → device | `{"v":2,"status":"probing","epoch":123}` heartbeat every 1 s |
| `0x21` | host → device | config command JSON, without an epoch prefix |
| `0x22` | device → host | config reply JSON |
| `0x23` | host → device | `epoch(u32 LE)` followed by ASCII `<bind>`: `ca:<host>`, `none`, or 64 lowercase hex chars |

`crc8` is CRC-8/SMBUS (polynomial `0x07`, init `0x00`, no reflection) over
the payload only. A frame with a bad CRC is dropped and the parser resyncs on
the next `A5 5A`.

A heartbeat reports physical bridge presence separately from server availability.
`status` is `disabled`, `probing`, `ready`, or `unavailable`; `epoch` is a nonzero
32-bit integer, changed for every fresh authentication attempt. Firmware accepts
only v2 heartbeats and routed frames whose epoch matches the current heartbeat.
There is no legacy USB protocol path. Logs and config remain available even when
routing is disabled or the host cannot reach the server.

**Routing: Wi-Fi is primary, USB is the fallback.** The device sends a routed hello over USB only when it wants
that route, and the hello is the demand signal: the bridge holds no upstream connection, timers or status churn
while it is in standby (`disabled`/`unavailable`/idle heartbeats keep the same epoch), and opens the upstream
WebSocket only after a hello. The device wants USB when it has no Wi-Fi session: 12 s after boot (`USB_BOOT_GRACE_MS`,
time for Wi-Fi to come up first) or 3 s after a Wi-Fi session was lost (`USB_DROP_GRACE_MS`). A device that is
online over Wi-Fi therefore causes no upstream traffic from the bridge at all. A bridge WebSocket opening is not
sufficient: only an authenticated server `welcome` makes the bridge `ready`.
With USB transport v2 the bridge relays the device v5 handshake while `probing`: the server's `challenge`
and `pair` reach the device as routed JSON of the current epoch, and the
device's `auth` goes upstream (one per challenge). A `pair` with a code lifts
the 8 s authentication bound (a human is approving). An identical hello while
probing replays the unanswered `challenge` or the pending `pair`; every new
server connection authenticates afresh, and only a connection that is already
`ready` answers repeated hellos with the cached welcome.
The bridge sends the ready heartbeat before that matching welcome. Firmware
promotes the USB route, posts link-down/link-up in order, and then delivers the
welcome and subsequent frames. A delayed welcome from a failed or previous epoch
cannot select USB. Wi-Fi likewise becomes active on its server welcome, before
the application receives that message.

**Failback.** While USB is in use the device keeps retrying Wi-Fi. When its Wi-Fi session authenticates, the server
replaces the session it holds for the device (the one that came through the bridge) and closes that socket with
`4003` (`CLOSE.REPLACED`). The bridge treats `4003` as a release, not a failure: it drops the upstream, resets its
backoff and returns to standby, and the device carries on over Wi-Fi with no gap beyond the reconnect itself.

USB is the active route only while it is authenticated and fresh and Wi-Fi is not. On bridge failure or disabled
routing, firmware uses available Wi-Fi; missing heartbeats expire after 2.5 s
(the manager polls at 200 ms). USB authentication is bounded to 8 s (started when the upstream opens, not while
the bridge idles), with retry backoff of 2/5/10/30 s. WebSocket ping/pong detects a hung USB upstream in at most
8 s. Repeated identical device hellos reuse the current connection and cached
welcome instead of replacing the server session. Wi-Fi connection attempts are
bounded to 22 s for WSS, with 3/6/12/24/30 s retry backoff; failed client allocations are
released. WSS starts only after SNTP supplies usable certificate-validation time.
For tests, the `sim` config event `wifi` (`ms`, default 30000) pauses the Wi-Fi session for that long without saving
anything, which hands the device to USB; `ms: 1` ends the pause at once.

### Transport binding over USB (`0x23`)

The USB cable is a physically trusted link; the bridge is the TLS client. It opens the upstream WebSocket for
the device's server setting and tells the device the transport binding it observed, so the server rules above
hold unchanged for tethered devices:

* device server empty → UDP discovery from the host, then `wss://<ip>:<port>/kubik/v1` without CA check, bind =
  SPKI SHA-256 of the certificate presented; `kubik://host[:port]` → the same without discovery; `wss://` →
  normal CA validation, bind `ca:<host>` (the host of the URL the bridge dials, normalised as above); `ws://` → bind `none`.
* The bridge sends exactly one `0x23` per upstream connection (every attempt uses a fresh epoch), after the
  upstream WebSocket opened and before any routed server frame of that epoch (before the relayed `challenge`;
  serial writes are FIFO). A LAN server that presents no certificate makes the attempt `unavailable`.
* The device keeps the bind of the current epoch only and answers a routed `challenge` only if a bind for that
  epoch arrived (otherwise the 8 s authentication bound retries). In LAN mode the bind must be 64 hex chars and
  is checked against the stored pin exactly like a direct TLS connection (mismatch: "server key changed", no
  `auth`; no pin: TOFU, persisted after `welcome`). In CA or dev mode the device stores no pin and signs the
  reported value as is.

By default the bridge uses the device's configured server. `--server URL`
or `KUBIK_SERVER` explicitly overrides it with the same rules for its scheme
(`kubik://`, `wss://`, `ws://`). There is no implicit localhost destination. Invalid
URLs report an actionable unavailable status while config/logs keep working. `ws://`
and `wss://` URLs require a hostname and absolute path, without URL credentials,
query, fragment, control characters, or spaces; the device limit is 127 bytes.

For deterministic Wi-Fi-only testing while retaining USB configuration and logs,
start the bridge with `--no-usb-route`, or toggle its local control endpoint:

```sh
curl -X POST http://127.0.0.1:18791/usb-route \
  -H 'content-type: application/json' -d '{"enabled":false}'
```

Sending `{"enabled":true}` starts a fresh USB authentication probe.

### Config commands (`0x21` → `0x22`)

| Command | Reply |
|---|---|
| `{"cmd":"agent_host"}` | Disable Muse selection and reboot using the saved agent server; an active conversation refuses the operation; settings and keys are retained |
| `{"cmd":"info"}` | device id, fw, P-256 public key, Wi-Fi SSID and `wifi_connected` (IP acquired), server URL, volume, selected `via`, `online` (server welcome processed), battery, heap; while setup is active, its temporary `setup_ap` and `setup_pass` |
| `{"cmd":"set","ssid":"…","pass":"…","url":"","name":"…","volume":70}` | `{"ok":true}`; any subset of keys, saved to NVS; `url` is the server setting (empty, `kubik://…`, `wss://…`), saving it clears the LAN pin |
| `{"cmd":"reboot"}` | `{"ok":true}`, then restarts |
| `{"cmd":"reset"}` | factory reset: `{"ok":true}` and a restart into first-run setup; saved Wi-Fi networks, server, LAN pin, name, volume, brightness, guide completion and device key are erased; new pairing is required; the compiled character is unchanged; `{"ok":false,"error":"…"}` if NVS refuses (run it again) |
| `{"cmd":"sim","ev":"ptt_down"}` | test hook: injects `ptt_down`, `ptt_up`, `tap` (`x`,`y`), `pet`, `shake`, `pickup`, `boot`, `pwr`, `setup`, `menu`, `mode` (`mode`, `ms`), `pair` (`code`), `stats` (сразу печатает статистику кадров) |

The saved home Wi-Fi password is write-only: `info` never returns it.

TESS diagnostics: `wake_listening` is local recognition, `auto_recording` is an
uploaded automatic turn. `mic_open` denotes recording; it is false for passive wake.
`wake_probability` is the latest 5-invoke mean (0..255), `wake_peak_probability`
is the peak since boot; `wake_inference_average_us` and `wake_inference_max_us`
measure wall time including preemption since boot. PLUSH reports zero/false wake values.
`screen_dimmed` reports the dimmed interactive state; `screen_dark` reports panel-off.
The temporary setup AP password is shown only while setup is active.

Wi-Fi onboarding uses the device's password-protected temporary AP and its
page at `http://192.168.4.1/`. A missing SSID opens setup immediately; after
a prolonged Wi-Fi failure it reopens setup, and the owner can open it from the
device menu. The page submits the home 2.4 GHz SSID/password and the optional
server setting together (empty = OpenClaw in the same network). The candidate Wi-Fi credentials remain in RAM while the
device checks that it can obtain an IP; only a successful check replaces the
atomic NVS connection record. Server connectivity alone does not trigger AP
setup. Pairing approval happens later through OpenClaw.

## Server behaviour (openclaw-kubik 0.6.2)

These are clarifications made by the server implementation. Apart from the v5 handshake above, none of them
changes a frame format.

**Close codes.** `4001` means a bad signature (including a transport binding
mismatch), a disabled device, or an unsupported firmware version; the server sends `{"t":"error","code":"unauthorized"}`
just before closing. The other codes are:
* `4002` is a protocol error. Examples: the first frame is not `hello`, there is no `hello` or `auth` within 3 s,
  an invalid key, a `hello` whose `v` is not 4 or whose `name`/`fw` is invalid, a frame other than `auth` after `challenge`, a frame other than `ping` while waiting for pairing,
  the JSON is invalid, a field is invalid, a JSON frame is over 4 KiB, an audio frame is over 8 KiB, the
  audio payload has an odd length, or the device sends `hello` twice.
* `4003` means the connection was replaced by a newer authenticated connection of the same device.
* `4004` (pairing timeout): not approved within 10 minutes; reconnect to keep waiting.
* `4005` (pairing busy): too many devices are waiting for approval or too many
  unauthenticated sockets are open; retry later.
* `1011` means the server could not reach OpenClaw's pairing store.
* `1001` means the server is stopping.

**`gen` numbering.** `gen` runs 1..255 and wraps from 255 to 1. **`0` is never used**, so a device can
use 0 as "no active gen". While the server process runs it keeps counting per device across reconnects,
so a reconnect never reuses the gen of audio that may still be buffered.

**One `gen` per spoken reply.** The agent may deliver a reply in several blocks, and all of them are
spoken under one `gen`. The server sends `speak` (and `state speaking` right after it) before the first
audio. It sends `speak_end` after the last block has been synthesised and sent.

**Emotion timing.** The emotion of the reply's first sentence is sent just before `speak`. Later
`emotion` frames are sent at the estimated moment their sentence starts playing. Because the server runs
up to ~800 ms ahead of playback, such an `emotion` can arrive **after `speak_end`** of the same `gen`.
`emotion.ms` is not used by the server.

**`played` and idle.** The server does not start a new `gen` until the previous one is `played`. If
`played` never arrives, the server falls back to the estimated remaining playback time plus 2.5 s. It
sends `state idle` after `played` (or the fallback), after `error stt_empty`/`stt_failed`, and after a
cancel.

**Cancel and barge-in.** Unscoped `cancel` (with or without `gen`, without `turn`) and a new manual `ptt on` while the server is
thinking or speaking both stop the current speech at once. The server sends no `speak_end` for the
cancelled `gen`; after `cancel` it sends `state idle`. The agent run itself is **not** aborted: it
finishes in the background and its reply stays in the conversation history. Its remaining reply blocks
are dropped and logged, not spoken, and any tool side effects still happen.

Automatic `ptt on` is rejected as busy during active output or agent work. A matching
`cancel.turn` abandons only that recording/transcription; it does not interrupt output or notifications.

**Proactive speech (`kind: "notify"`).** A notification waits for a reply that is being spoken to
finish. It never starts while the key is held. It gets its own `gen`.

**Audio frames.** Frames whose `turn` differs from the current `ptt on` turn are ignored. At most 60 s
of audio per utterance is used, and a lost `ptt off` is assumed after 65 s. Speech PCM chunks are
≤ 4800 bytes (100 ms) before IMA encoding. Sent-but-unconfirmed speech is limited to 800 ms.

`welcome.volume` reports the effective volume (the configured channel override, or the device's actual
`hello.volume`). Local volume changes are sent as `device_state` so output routing uses the current value.

## Agent controls and speech capabilities (0.6.2)

Authenticated devices send `{"t":"device_state","volume":19}` after a local volume change.
`hello.volume` has the same integer range 0–100. Unknown volume never enables synthesis. The server
speaks only at volume ≥20 with configured TTS; otherwise the reply is sent as `text`. STT is independent
of this decision. The server sends `capabilities` after welcome with `stt.available`, `tts.available`
and `voice_mode` (`classic`, `realtime`, or `live`). Realtime closes KEY recording after local VAD. Live stays active until an explicit
stop, menu, screen off, disconnect or error; Tess can start it with Hi Tessa.
Live suspends the local wake detector while its conversation is active. These fields contain no provider credentials.
If STT is unavailable, the device must not capture microphone audio; the response explains how to configure it.
These are configuration capabilities, not a claim that a remote provider has answered a paid probe.

For native voice the server may send `{"t":"input_end","turn":4,"session":"s-3"}`.
The device accepts only a matching current turn on its current link session. RX invalidates
that capture epoch without waiting for the mic callback; the app drains capture and closes
the listening state before replying. A stale turn/session cannot stop a newer recording.
This event does not send another PTT-off; locally detected pause sends the normal scoped
PTT-off once. Classic mode ignores native input-end. See [native voice](native-voice.md).

To open the model picker, send `{"t":"agent_options","target":"agent","rid":1,"cursor":0}`. To select a model, send
`{"t":"agent_model","target":"agent","rid":2,"cursor":2,"id":"provider/model"}`. `rid` is 0–65535; `cursor` is the page number 0–255
and is required when selecting a model so the reply preserves the current page;
model IDs contain at most 160 characters without control characters. Both requests return `agent_options`
with the same required `target` (`agent`, `mode`, `voice`, `stt`, or `tts`), `rid`, `cursor`, `total`, current `model`, at most four `models:[{id,label,available?}]`, and
`stt`/`tts` metadata `{available,provider,model}`. Labels contain at most 80 characters. A safe `error`
code can replace the data: `busy`, `timeout`, `unsupported`, `unavailable`, `invalid_model`, `denied`.
The device ignores stale request IDs and shows a selection only after acknowledgement.

The adapter supplies a bounded catalog of at most 128 real models and validates selection against
its current allowlist. The `agent` target changes only this device's agent session. Speech targets use the configured host
speech catalog and store choices by account, device and pairing fingerprint.
`mode` returns three rows with IDs `classic`, `realtime`, `live`, explicit boolean `available`,
and the selected mode in `model`; cursor is zero. Unavailable modes cannot be selected.
The device displays `classic` as STT. After saving a mode, the host refreshes its engine,
returns the confirmed catalog, then sends updated capabilities. The device requests the next
catalog on its app task, never synchronously inside a WebSocket receive callback.
`voice` is the single Realtime/Live model; `stt`/`tts` apply only to Classic.
Wrong-mode targets are rejected as unsupported. Each mode retains its own model selection.
A host without native voice advertises Realtime/Live as unavailable. A host without
speech model selection returns no speech choices. Host mode config is only the default;
a saved device mode takes precedence, without changing other devices or agent sessions.
The device keeps the active catalog separately from its agent model and rejects mismatched targets. One control request is in flight
per connection; model changes cannot overlap a recorded/active reply. Recording also waits for an outstanding
model change. A timed-out operation remains bounded until its underlying request settles; the UI refreshes
state to learn the actual result. No keys, provider credentials or session transcripts enter the catalog.

Microphone RX/ADC are enabled for PTT recording and, only in TESS firmware, eligible
local Tessa recognition. Passive wake capture is distinct from `mic_open` and the
listening UI. There is no acoustic steering, gain-isolation hook or microphone-edge overlay. Synthesizing and
playing replies does not enable the microphone. Live transcription, when configured, starts its provider
socket at PTT-on, streams only that recording, commits at PTT-off, and dispatches the matched final transcript
to the same agent session. Partial transcripts never execute agent commands or tools.

## Independent agent host and battery polling

The protocol is agent-independent. See [Agent SDK](agent-sdk.md) for adapters, setup, pairing and activity/cron snapshots. Both hosts reuse the same device authentication and speech transport.

On battery, with a dark screen and no conversation/setup/work, firmware stops the Wi-Fi radio. A connection window begins after the first 20-second off interval and then every 60 seconds, lasting at most 40 seconds. Join including scan is capped at 10 seconds, discovery at 3 seconds and WSS at 22 seconds; one WSS attempt is allowed per window. The timing budget leaves 2 seconds for event delivery/wake within the minute on a healthy reachable selected or scan-visible saved network with an already synchronized clock. First-time provisioning/NTP synchronization is separate from the battery polling deadline. A stalled/unavailable network retains the queued item. A new active agent-work snapshot or actual notification wakes the screen. Cron counter snapshots update the indicator without waking or resetting the screen idle timer. User input or charger insertion restores continuous connectivity. Radio-off intervals cannot receive a push. Server notifications therefore use a durable bounded queue (8 per device, 128 total, 24-hour TTL), bound to the approved identity fingerprint. They drain on authenticated reconnect, wait behind active PTT and retain pending items if disconnected before output. An item is removed after device acknowledgement, explicit user cancellation, identity revocation or its 24-hour expiry. Spoken notifications await matching `played` confirming the full sent PCM duration; text-only notifications await `shown` for the final card preview receipt (at most 1000 UTF-8 bytes); spoken output uses the full accepted text. A disconnect or missing ACK retains the item for the next authenticated connection; ambiguous partial delivery may be repeated. `shown` confirms installation into the device UI model, not panel scanout. Firmware binds text receipts and audio generations to the connection that supplied them. Delayed END/ACK from a prior connection cannot truncate or acknowledge a replacement stream, even if the 8-bit generation repeats. Delivery latency includes the next connection window and network/API delay.


### Final text-card receipt

Server text updates carry `receipt`, a positive uint32 scoped to the authenticated
connection. The device sends `{"t":"shown","receipt":N}` after the full card is
installed into the face model and the screen is woken. Progressive updates have
new receipts; only the final matching receipt completes a text-only notification.
A stale or unrelated receipt cannot complete delivery. Audio notifications complete
with matching `played(gen, ms)` covering the full sent PCM duration instead. No ACK means retained queue entry, one delivery attempt
per connection; user cancellation intentionally consumes the notification.


### Bounded radio diagnostic (USB config only)

USB `info.audio_dozing` exposes the codec/TX doze reservation; it is diagnostic, not a battery-current measurement.

`{"cmd":"sim","ev":"power-network","ms":120000}` temporarily bypasses only
charger/USB power gates in the copied sleep and CPU-light-sleep inputs. The real screen must be dark
and the device idle, with no setup/AP/recording. It invokes actual Wi-Fi driver
stop/resume through the ordinary coordinator. Driver state, coordinator phase,
transition count and diagnostic deadline window are exposed by `info`.
`network_diagnostic` reports the window; the override also requires a fresh USB host heartbeat.
`cpu_light_sleep` reports permission, not measured sleep residency. When permitted,
C6 may suspend USB-Serial/JTAG; reopen the bridge after the radio wakes. Expiry (at most
120 s), loss of the USB host or screen wake restores normal power inputs.
This fixture does not override PMIC values, persist settings or measure battery draw.

Active microphone capture drains DMA, removes DC and encodes IMA once in a short
priority-9 task. A 30-frame (1200 ms) packed queue passes original codec bytes to
priority-7 TLS work; local VAD uses their decoded PCM. Close drains the queue,
while a terminal provider/VAD event discards frames from its old capture epoch.
Overflow aborts the turn. The queue and decode scratch occupy 16560 bytes of the
reserved turn workspace; no extra PCM queue is allocated. The 4096-byte worker
stack temporarily leases LP SRAM with MALLOC_CAP_RTCRAM (CPU-only, not DMA),
leaving ordinary SRAM for Wi-Fi. After the worker joins, its stack is freed and
both queue/scratch are cleared, including startup failure cleanup. TLS cannot use
that workspace until the worker joins; playback then reuses it for full records.
Passive wake inference stays at priority 6 and uses raw PCM. Listening animation
targets 24 FPS; other states remain 30 FPS.
Live uses sixteen paired 40 ms mic/reference frames (15552 bytes, 640 ms) in
separate internal memory, with no local PCM decode. Its sender stack uses 3072
bytes of LP SRAM. Live mic delivery shares priority 6 with playback progress;
WebSocket RX stays at 7, below display 8. The receiver can consume queued speech
before another microphone send, and backlog cannot starve playback credit.
ChaCha20-Poly1305 and hardware AES are enabled, with normal SDK cipher negotiation.
The stock SDK masks and frames WebSocket messages. A bounded 1024-byte write
coalescer combines its header and body: a Live 968-byte microphone/reference
payload occupies one TLS content write. Larger messages retain the same wire
bytes across bounded writes. A short write fails the connection; no partial
frame is reported as delivered. The common SDK client lock remains enabled.
Wi-Fi uses four dynamic RX buffers. Copying RX into lwIP releases the driver
buffer before socket consumption, leaving space for ACKs during duplex sends.
The TCP receive window holds at most 2880 bytes (two 1440-byte segments).
A 485-byte compressed microphone frame fits a single WebSocket payload. A complete
binary message is consumed synchronously from the SDK's RX buffer without a duplicate
allocation. Fragmented messages and JSON use bounded temporary assembly storage.

Speech memory: the firmware allocates its entire 900 ms ADPCM ring in three fixed 3600-byte blocks (two in LP SRAM, one in ordinary internal heap)
or refuses playback. It starts with 600 ms buffered and resumes a starved
stream after 300 ms. Short replies can start after a bounded 600 ms wait from their first
PCM, even without an end marker. Starvation counters describe pauses followed
by more PCM; a terminal quiet tail is excluded. One reservation leaves smaller heap regions available to Wi-Fi/TLS.
TLS TX scratch uses a 1024-byte content limit; ESP-IDF fragments larger writes.
RX retains its 16 KB content limit. TX and ordinary speech RX use separate
reserved slots during Live: a 1536-byte fixed TX slot and the full RX reservation. During Wi-Fi speech it reserves 17408 bytes for
RX records after releasing wake-word memory;
an in-flight record keeps its reservation until the TLS reader releases it. Neither reservation
remains in the idle wake-listening path. Native PCM deltas coalesce into bounded 400 ms queue
items, so a burst of small provider deltas cannot exhaust a segment-count limit.
A single native callback may span multiple seconds: the same 60 s pending-PCM
limit bounds individual callbacks and the combined stream/pacer queue.
The pacer coalesces small PCM callbacks into 100 ms packets. A partial packet waits
at most 100 ms from its first bytes; a marker/end boundary flushes it immediately,
subject to device credit. This reduces small writes over TLS without enlarging the ring.

The production speech pacer caps sent-but-unconfirmed PCM at 800 ms,
including audio still in network transit. Matching monotonic `progress` receipts
release this window; time alone cannot release it. Five seconds without forward
playback progress while the window is full closes the stalled connection.
The 900 ms firmware ring therefore retains 100 ms of capacity headroom.
For user turns, TLS memory is reserved after the wake model stops and before VAD
allocation; independent turn, speech and in-flight record ownership release it
only when all three are done. This avoids fragmenting the required contiguous
record allocation at speech startup without holding it during idle wake listening.


### Native microphone handoff and continuous GPT Live

`ptt on:true` starts one SDK connection. Live sends mic/reference packets immediately
in capture order, including silence and during playback. Only cleaned mono PCM enters
the provider. Wake preroll occurs with no speaker output and uses a silent reference.
KEY release does not end Live; explicit stop sends `cancel` for that turn.
`live_input {turn:u8,on:boolean}` and its matching `live_input_ack` confirm startup
(on:true for Live), or close Realtime input (on:false). Stale turns are ignored;
the ACK deadline is 3 s. Live does not pause input for a reply or a playback receipt.
Each output segment has its own `gen`; `played` confirms the actual I2S tail.
`speak_cancel {gen:u8}` flushes queued speech for that generation. The device replies
`cancelled {gen:u8}` after stopping its matching stream; an already-drained generation
is also acknowledged. Foreign connection/generation commands cannot stop newer output.
The host waits for cancellation ACK (3 s) before queuing a replacement; capture continues.
The provider's continuous output has no public response boundary: 3 s without audible
PCM retires an output segment, preserving the SDK connection and microphone. Limits:
30 min per session, 120 s per consultation. `input_end` closes the conversation,
not a single phrase. Realtime retains its separate record/VAD/send cycle.

### Раздельная громкость устройства

Поле `volume` в hello, device_state, welcome и set — громкость речи агента,
0–100. Порог TTS 20 относится к ней. Interface хранится только на устройстве:
host не меняет её при welcome или изменении volume. Локальная USB-команда
`set` принимает `ui_volume` (целое 0–100); `info` возвращает `ui_volume` и
`sound_menu`. Это диагностический локальный интерфейс, не новый сетевой контракт.

## Нативный Muse

Muse использует собственный BLE v5 + TLS/WebSocket/Noise XX контракт, не `/kubik/v1`.
Локальный backend приводит его текст, возможности и состояния к callbacks приложения.
USB-команда `muse_pair` с полем `sdk_token` начинает сопряжение и перезапуск, только
когда разговор не активен. Ответ сообщает лишь успех/перезапуск. Она предназначена
для локального обслуживания; основной ввод ключа — мастер на телефоне. Никогда
не сохраняйте запрос с личным ключом в логах. См. [Muse](muse.md).
