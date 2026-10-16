# Approve your Kubik

Kubik always proves possession of its device key. A new key also needs explicit
approval before it can use your agent, tools or conversation. Compare the code on
the device with the request before approving. A normal firmware update keeps the
key and approval; a factory reset requires a new approval.

## Telegram: one button

With an eligible Telegram owner chat, your existing agent bot sends **Connect
kubik-…?**, the code and **Allow**. Match the device code and tap **Allow**. This
runs no model and uses no speech provider. Buttons expire after at most ten minutes
and cannot be reused. Requests do not repeat on every Wi-Fi reconnect.

**OpenClaw:** Telegram must be enabled. Add your private Telegram user ID explicitly
to `commands.ownerAllowFrom`, e.g. `"telegram:123456789"`, preserving other owners.
A Telegram `allowFrom` entry alone grants chat access, not pairing administration.
The same owner ID must also be explicitly allowed by that Telegram account’s `allowFrom`. Inline buttons must be enabled for owner DMs. With multiple bot accounts, the
request remains bound to the bot account that delivered it.

**Hermes:** enable its existing Telegram platform and configure a private
`TELEGRAM_HOME_CHANNEL` / Telegram `home_channel` belonging to its authorized
owner. Group home channels are not used for approval. The native plugin attaches
a scoped callback handler to that bot; no second bot or process is started.

Only the owner may approve. Group callbacks, different accounts, revoked authority
and expired/consumed requests do not grant access. If the bot restarts while a
request is pending, reconnect Kubik to obtain a fresh actionable prompt.

## Without Telegram

Telegram is optional. If no eligible owner chat exists or sending fails, Kubik
shows server approval instructions instead of claiming a Telegram request exists.

On the OpenClaw server:

```sh
openclaw pairing approve kubik CODE
```

The OpenClaw Web UI also has **Channels → Pairing requests**.
For Hermes, use the exact profile-aware approval command printed by its installer:

```sh
/path/to/hermes/python /path/to/hermes-home/plugins/kubik/pair.py CODE
```

The short screen command assumes you are already in that Hermes Python environment;
`<home>` means that agent's actual home directory. See [Hermes setup](hermes.md).
Never disable pairing or share agent provider keys to bypass approval.
