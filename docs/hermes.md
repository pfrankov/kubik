# Hermes directly over Wi-Fi

Kubik connects to a platform plugin **inside your existing Hermes Gateway**.
No Kubik companion process, home-computer relay, SSH tunnel or Hermes API Server
is required. The agent's model, tools, credentials and conversation history stay
on the Hermes machine. Kubik has its own device conversation, not your current CLI conversation.

This integration targets Hermes 0.21.4 and Python 3.11+. It uses Hermes's native
Classic STT/TTS pipeline. Hermes 0.21.4 does not provide Realtime or GPT Live
through its Gateway; those modes remain available through OpenClaw.

## Install once, on the Hermes machine

1. Install and configure Hermes using its [official guide](https://hermes-agent.nousresearch.com/docs/getting-started/installation).
   Check a normal text conversation and keep its Gateway installed.
2. Download the Kubik repository from the GitHub link supplied with your order.
   Run the installer as the same OS user that runs Hermes:

   ```sh
   python tools/install-hermes.py --home ~/.hermes
   ```

   Use the Python environment of your Hermes installation. Dependencies are
   `aiohttp`, `cryptography`, `PyYAML` and `ffmpeg`; Hermes's standard environment
   already includes the Python libraries. Install ffmpeg if it is absent.
   For a service with a custom `HERMES_HOME`, pass that directory explicitly.
   The installer preserves the other platform settings and makes a private
   configuration backup. It never stops the Gateway.
3. Restart Hermes Gateway when its conversations are idle:

   ```sh
   hermes gateway restart
   ```

   If you run Hermes through a custom systemd service, restart that existing
   service instead. Do not start a second Gateway against the same home.

The plugin is installed in `HERMES_HOME/plugins/kubik`. It adds one TLS listener
on TCP **18793**, separate from OpenClaw's port 18790. Open this port only on the
Hermes machine/firewall that the device needs to reach. Authentication requires
both proof of the device's private key and your local pairing approval.

## Connect and approve

1. Connect Kubik to 2.4 GHz Wi-Fi using its phone setup page.
2. Choose **Hermes** and enter **`kubik://HOST:18793`** in Server address.
   HOST is your Hermes machine's reachable LAN IP or public IP. A domain is
   optional. A Tailscale-only `100.x` address requires a route from the device's
   Wi-Fi; installing Tailscale on your phone does not create that route.
   This plugin does not provide blank-address discovery.
3. Tap **Connect Kubik**. On the Hermes machine, approve the matching code
   displayed on the device:

   ```sh
   python ~/.hermes/plugins/kubik/pair.py CODE
   ```

   Run this as the Hermes service user, with the same `HERMES_HOME`.
   Codes expire after ten minutes. The existing socket completes pairing.

Keep `HERMES_HOME/kubik` when updating or restoring Hermes: it contains the
pinned TLS identity and device approvals. Startup restores owner-only access
(0700 for this directory, 0600 for its state files) without changing the key
or approvals. State files must be regular files, not symbolic links. Writes
replace complete files atomically; a failed write preserves the previous file.

As the Hermes service user, revoke a paired device by removing its exact
`device-id:fingerprint` entry from `HERMES_HOME/kubik/approved.json`, keeping
the remaining entries as a JSON list.
The current connection stays open, but new recordings, agent handoffs and
new outbound replies are denied without restarting Hermes. Playback already in
progress may finish. A turn already accepted by Hermes is allowed to finish in
its agent conversation.

Use `kubik://`, not raw `wss://`, for this listener: Kubik pins its TLS key.
A plain `wss://` address expects a publicly trusted certificate. Saving a new
Server address clears the old pin; a changed key otherwise requires explicit
reconfiguration. Provider keys are never entered on Kubik.

## Configure speech in Hermes

Use Hermes's [voice guide](https://hermes-agent.nousresearch.com/docs/user-guide/features/voice-mode)
and its configuration commands to select STT and TTS providers, models and
credentials on the agent machine. Restart the existing Gateway after changes
when idle. This integration reads those settings directly; there is no second
speech credential file or API key for a Kubik host.

You can ask your Hermes agent to configure its speech providers, identifying
which provider/model you want and the location of its existing protected
credentials. Review the resulting configuration; do not paste secret keys into
an ordinary chat or the device's Server field.

With ready STT, KEY starts a voice request; Tess also supports Hi Tessa.
Voice-activated input ends after VAD silence. Manual Classic recording follows
KEY hold/release or tap controls, with a 60-second limit; a pause does not end it.
With ready TTS and Speech volume at least 20, Hermes speaks its
reply; otherwise Kubik shows text. Realtime/Live and the device model pickers are
not implemented by this Hermes platform. Change the agent and speech models in
Hermes itself. Disconnects do not reset its device conversation.

Only one authenticated connection is active per device. A reconnect replaces the
previous socket, including overlapping reconnects. Queued speech rechecks that
connection, cancellation and the current volume before starting playback, so an
interrupted reply does not resume when an earlier audio fragment finishes.
Text, voice and typing updates are sent only after `welcome` and capabilities;
delivery requested during that handshake returns a retryable connection error.

## Extend or troubleshoot

The implementation is [hermes-kubik](../hermes-kubik/adapter.py): `adapter.py` owns the native
platform lifecycle, `security.py` authentication/pairing, `codec.py` IMA audio and
`speech.py` paced playback. It does not call Hermes's Responses API.

Check `hermes gateway status`, the existing service log and port 18793. A pairing
code means networking and the signed TLS handshake succeeded; a successful
connection alone does not prove provider access. Keep paid conversation tests
explicit. Local acceptance uses mock providers and real TLS without billing.
