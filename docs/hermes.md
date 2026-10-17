# Hermes directly over Wi-Fi

Kubik is a native platform **inside your existing Hermes Gateway**, not a second
agent, API server, companion daemon or SSH relay. OpenClaw remains an independent
choice. Model settings, tools, credentials and history stay in Hermes; each paired
Kubik gets its own conversation, separate from the CLI and other devices.

The compatibility target is **Hermes Agent v0.22.2**, upstream commit
`d923a5eb487a17c0a68801bba9ef0f56c81040a8`, released 15 October 2026.
Its Python environment must use **Python 3.14**. The automated native lane pins
Python 3.14.8; [the machine-readable pin](../tools/hermes_native/upstream.json)
and [release](https://github.com/NousResearch/hermes-agent/releases/tag/v0.22.2)
identify exactly what was tested. Later releases require rerunning that contract,
not merely changing the version label.

## Prepare the existing agent

Install Hermes using its [official guide](https://hermes-agent.nousresearch.com/docs/getting-started/installation)
and verify a text conversation first. Run these commands from the downloaded
Kubik repository, **as the user who runs Hermes**. Replace the two paths with the
actual interpreter and home of that agent or named profile:

```sh
HERMES_PY=/absolute/path/to/hermes-agent/.venv/bin/python
HERMES_HOME=/absolute/path/to/your/hermes-home
uv pip install --python "$HERMES_PY" -r hermes-kubik/requirements.txt
"$HERMES_PY" tools/install-hermes.py --home "$HERMES_HOME" --check
"$HERMES_PY" tools/install-hermes.py --home "$HERMES_HOME"
```

The additional dependencies are aiohttp and Tornado `>=6.5.9,<7`. The Tornado
floor fixes the pre-authentication query-field denial of service in Telegram's
webhook listener ([GHSA-3hv7-mjh2-fv65](https://github.com/tornadoweb/tornado/security/advisories/GHSA-3hv7-mjh2-fv65)).
The same floor also includes fixes for
[CurlAsyncHTTPClient decompression](https://github.com/tornadoweb/tornado/security/advisories/GHSA-chx6-46f5-w4vp)
and [StaticFileHandler symlinks](https://github.com/tornadoweb/tornado/security/advisories/GHSA-c2m8-h5v5-343r);
those uses are not established for the Kubik webhook path.
The installer uses the existing
Hermes `ruamel.yaml` and cryptography libraries, not an assumed system PyYAML.
It fails before writing when run in the wrong interpreter or an incomplete
Hermes environment. Install **ffmpeg on the agent machine** for speech playback:
`brew install ffmpeg` on macOS, or the ffmpeg package from your Linux distribution.
The read-only `--check` reports whether ffmpeg is available.

Installation preserves other platforms, models, credentials, YAML comments and
an already configured Kubik port/bind address. It enables the directory key `kubik`
(migrating the old `kubik-platform` selection so Hermes PM includes its dependencies), keeps
other enabled/disabled plugins unchanged, and creates the private first-install
backup `config.before-kubik.yaml`. Writes are staged; a normal failure replacing
the configuration restores the previous plugin. A concurrent installer or changed
configuration is rejected instead of overwriting it. If another writer occupies
`config.yaml` during commit, its version remains active and the pre-install version
remains in the private backup. `--check` writes nothing.
If installation was forcibly interrupted, run the same installer again without
`--check` before restarting the Gateway. Its private transaction journal restores
the preceding plugin/configuration or finalizes a completed commit. A configuration
created by another writer is preserved; when the first-install backup is older,
the displaced configuration is saved as `config.before-kubik-conflict.yaml`.
An occupied conflicting backup or an unrecognized replacement plugin stops recovery
and retains its journal and files for inspection instead of overwriting them.
The installer does not install Python packages, start/stop services, change providers or make model calls.
The printed approval command is shell-quoted and remains safe to paste when the
interpreter or Hermes home path contains spaces or shell metacharacters.

Kubik uses Hermes's native **in-process platform** loading. A profile with
`plugins.isolation` other than `in_process` is rejected without changing that
security setting. Do not silently weaken your policy to install the platform.

When existing conversations are idle, restart **the existing Gateway**:

```sh
hermes gateway restart
```

Use your established profile-aware service command instead when that is how
Hermes runs. Do not start a second Gateway against the same home. The plugin is
installed under `HERMES_HOME/plugins/kubik`; persistent state is under
`HERMES_HOME/kubik`.

## Listener and profiles

The native platform adds one TLS listener, normally **TCP 18793**, not OpenClaw's
18790. To change it, pass `--port 28793` and optionally `--bind 192.168.1.20` to the
installer. Omitted options preserve existing values; a new install binds
`0.0.0.0`. Ports must be integers from 1024 to 65535, and bind must be an IP address.
An existing profile-scoped `KUBIK_PORT` environment setting overrides the config
port at runtime; remove that override or use its value in the device address.

Use a unique port for each installed profile. The adapter captures the home of
its owning Hermes profile and scopes STT, TTS, native agent handoff and listener
configuration to that home without changing process-global environment variables.
The pairing command uses the home of the installed script, not whichever profile
happens to be active in a different terminal.

Allow inbound traffic only from networks that need to reach this listener.
A LAN or routed private network is preferable to public exposure. A domain is
optional. A VPN address only works when **Kubik's Wi-Fi network has a route to it**;
installing a VPN on the phone alone does not provide that route. Hermes does not
provide blank-address automatic discovery.

## Choose Hermes and pair

With an authorized private Telegram home chat, the bot sends a code and **Allow**
button. Match the code on Kubik and tap it. Without Telegram, a private owner chat
or successful delivery, use the installer command below. See [approval methods](pairing.md).


In the phone setup, select **Hermes**, then enter **`kubik://HOST:18793`** or the
configured alternative port. The page shows only Hermes installation and approval
instructions. The USB setup page offers the same explicit choice and address;
older URL-only USB commands retain the current integration and must still provide
a valid explicit address when that integration is Hermes. Changing the selected
integration clears the previous endpoint so an OpenClaw address cannot be reused
silently for Hermes or vice versa. Choosing OpenClaw again restores its
discovery/approval path without deleting saved Wi-Fi networks
or Muse credentials. The choice survives restart in the existing atomic NVS
record; firmware storage layout and record size are unchanged.
Typing or clearing an address also fixes the visible agent choice: a delayed
initial device response cannot switch that choice or restore an old endpoint.

HOST must be reachable from Kubik, never `localhost`. Use **`kubik://`**, not raw
`wss://`: this listener uses key-pinned TLS rather than a publicly trusted
certificate. Match the pairing code shown on the device before approving it.
Use the **exact interpreter and script path printed by the installer**, for example:

```sh
"$HERMES_PY" "$HERMES_HOME/plugins/kubik/pair.py" CODE
```

Codes expire after ten minutes. The existing socket completes pairing. Proof of
possession of the device's P-256 private key and a local approval are both required;
a claimed device name or allow-all environment variable is not sufficient.
The firmware's abbreviated `<home>` pairing hint must be replaced with the actual
Hermes home; the installer prints a ready-to-use absolute command.

Keep `HERMES_HOME/kubik` across plugin updates: it contains the pinned TLS key and
approvals. State is owner-only, regular files are required, and complete-file
writes are atomic. Do not erase device NVS or host identity as an upgrade step.
After a genuine host key change, investigate it before explicitly reconfiguring
the device; an unexpected key replacement is not accepted silently.

## Speech and native agent behavior

Configure providers, models and protected credentials through Hermes's
[voice guide](https://hermes-agent.nousresearch.com/docs/user-guide/features/voice-mode).
This integration uses **Classic: STT → native Hermes agent → TTS**. Realtime,
GPT Live and on-device Hermes model selection are not advertised. Model/tool
configuration stays in Hermes, and changing it does not create a separate Kubik
agent implementation.

Hold KEY, speak and release to send. Ready STT enables recording. Replies use
speech when native TTS and ffmpeg are available and Speech volume is at least 20;
otherwise they arrive as text. Readiness checks inspect settings, not provider
billing, quota, internet connectivity or response quality. A configured but failing
provider still reports its runtime error; no unapproved fallback provider is used.
Native Hermes prompts, tool behavior and notifications retain Hermes's policies.

The wire format remains protocol v5 with 24 kHz mono audio and IMA ADPCM frames.
Recording, playback credit, turn cancellation, generation IDs and text/audio
receipts stay in the existing bounded transport. Reconnecting the same approved
identity keeps its native conversation. A different device gets a separate session.

## Revoke, diagnose and update

To revoke, remove the exact `device-id:fingerprint` entry from
`HERMES_HOME/kubik/approved.json`, preserving the other entries as a JSON list.
No restart is needed. New recordings, handoffs and replies are denied; a turn
already accepted by Hermes or playback already underway may finish. A malformed
approval store is not treated as an allow-all policy.

For connection errors, check the intended profile, the installed plugin, the
actual listener port/bind address, firewall route and pairing code. A port already
in use causes startup to fail without leaving a half-started listener. For speech
errors, inspect the Gateway log and native provider settings; missing ffmpeg
makes speech output unavailable. Do not share secrets or private conversations
in diagnostic reports.

For an update, rerun `uv pip install --python "$HERMES_PY" -r hermes-kubik/requirements.txt`
with the interpreter used by Hermes, then repeat installation from the new Kubik
checkout using the same home. For a PM-managed environment, run
`hermes pm install --extra telegram` after reinstalling the plugin to adopt its
updated dependency declaration; `hermes pm repair` only replays the recorded graph.
Restart the idle existing Gateway to load the updated packages. Neither the installer
nor plugin discovery installs them automatically.
To remove the integration, disable `kubik` in that profile and restart its Gateway; preserve state for a
possible reinstall. Do not restore an old whole configuration over newer unrelated
Hermes changes. Physical device testing is separate from the automated contract.

## Reproduce the native compatibility test

```sh
# Requires git, uv and ffmpeg. The destination must not exist.
python3 tools/test-hermes-native.py --prepare /tmp/kubik-hermes-v0222
# Repeat against the same clean, pinned prepared checkout:
python3 tools/test-hermes-native.py --source /tmp/kubik-hermes-v0222
```

The runner creates a disposable Hermes home, strips inherited credentials, and
runs the **real plugin discovery, GatewayRunner and AIAgent**. Only external
LLM/STT/TTS HTTP endpoints are local deterministic fixtures; non-loopback
connections in the agent process are blocked. Tests cover signed pairing, voice,
odd-sample audio conversion, muted text, context across reconnect, separate devices,
revocation and concurrent profile scopes. They do not prove paid-provider quality,
physical microphone/speaker performance or every possible Hermes configuration.
[Acceptance](acceptance.md) distinguishes this native lane from the fast isolated
tests; [user scenarios](scenarios/hermes.md) describe the observable contract.
