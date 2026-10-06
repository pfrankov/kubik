import { defineChannelSetupContract } from 'openclaw/plugin-sdk/channel-setup';
import { CHANNEL_ID, channelSchema, DEFAULT_ACCOUNT_ID, inspectAccount, isDeviceId, normalizeDeviceId, resolveAccount,
  sectionOf, uiHints, withSection } from './config.js';

export const meta = {
  id: CHANNEL_ID, label: 'Kubik', selectionLabel: 'Kubik (voice desk device)', detailLabel: 'Kubik voice device',
  docsPath: '/channels/kubik', docsLabel: 'kubik',
  blurb: 'Push-to-talk voice desk assistant (ESP32-C6) that speaks replies aloud.',
  systemImage: 'waveform', markdownCapable: false,
};
export const capabilities = { chatTypes: ['direct'], media: false, threads: false, reactions: false, nativeCommands: false,
  blockStreaming: true, edit: false, reply: false };

const describe = (account) => ({ accountId: account.accountId, name: account.name, enabled: account.enabled,
  configured: account.configured, devices: [...account.devices.keys()], voiceProvider: account.voice.provider });

const setupDeviceIdInput = (input) => input.deviceId ?? input.device ?? input.userId ?? input.defaultTo;

/** Device id for `openclaw channels add`: explicit deviceId, or a kubik:<id> default target. */
export function setupDeviceId(input = {}) {
  const raw = setupDeviceIdInput(input);
  const id = normalizeDeviceId(raw);
  return isDeviceId(id) ? id : undefined;
}

/**
 * `openclaw channels add --channel kubik --enable` enables the channel for key-based pairing: the device shows a
 * code, `openclaw pairing approve kubik <CODE>`.
 * Mirrored in package.json.
 */
export const SETUP_FIELDS = {
  enable: { kind: 'boolean', cli: { flags: '--enable', description: 'Enable Kubik; approve devices through OpenClaw pairing' } },
  deviceId: { kind: 'string', cli: { flags: '--device-id <id>', description: 'Optional device id (e.g. kubik-b6c634) to set a name or default target' } },
  deviceName: { kind: 'string', cli: { flags: '--device-name <name>', description: 'Display name of the device (needs --device-id)' } },
};

const setupAdapter = {
  // The channel root is the single account; never promote it into accounts.default.
  configPromotion: 'preserve-root',
  singleAccountKeysToMove: [],
  resolveAccountId: () => DEFAULT_ACCOUNT_ID,
  applyAccountName: ({ cfg, name }) => (name?.trim() ? withSection(cfg, { ...sectionOf(cfg), name: name.trim() }) : cfg),
  validateInput: ({ accountId, input }) => {
    if (accountId && accountId !== DEFAULT_ACCOUNT_ID) return 'Kubik has one account; devices are configured under channels.kubik.devices.';
    const rawId = setupDeviceIdInput(input);
    if (rawId !== undefined && String(rawId).trim() && !setupDeviceId(input)) return 'Invalid device id (e.g. kubik-b6c634).';
    if (input.deviceName?.trim() && !setupDeviceId(input)) return 'A device id is required with a device name (e.g. kubik-b6c634).';
    return null;
  },
  applyAccountConfig: ({ cfg, input }) => {
    const section = structuredClone(sectionOf(cfg));
    section.enabled = true;
    const id = setupDeviceId(input);
    if (!id) return withSection(cfg, section); // pairing: the device registers itself
    const device = { ...section.devices?.[id], enabled: true };
    if (input.deviceName?.trim()) device.name = input.deviceName.trim();
    section.devices = { ...section.devices, [id]: device };
    section.defaultTo ??= `${CHANNEL_ID}:${id}`;
    return withSection(cfg, section);
  },
};

export const setupPlugin = {
  id: CHANNEL_ID, meta, capabilities,
  // channelPlugin adds notifyApproval (it needs the running server).
  pairing: { idLabel: 'kubikDevice', normalizeAllowEntry: (entry) => String(entry).trim().toLowerCase() },
  reload: { configPrefixes: [`channels.${CHANNEL_ID}`] },
  configSchema: { schema: channelSchema, uiHints },
  config: {
    listAccountIds: () => [DEFAULT_ACCOUNT_ID], defaultAccountId: () => DEFAULT_ACCOUNT_ID,
    resolveAccount: (cfg, accountId) => resolveAccount(cfg, accountId),
    inspectAccount,
    isConfigured: (account) => account.configured,
    describeAccount: describe,
    resolveDefaultTo: ({ cfg }) => {
      const to = sectionOf(cfg).defaultTo;
      return to ? `${CHANNEL_ID}:${normalizeDeviceId(to)}` : undefined;
    },
    setAccountEnabled: ({ cfg, enabled }) => withSection(cfg, { ...sectionOf(cfg), enabled }),
    deleteAccount: ({ cfg }) => {
      const channels = { ...cfg.channels };
      delete channels[CHANNEL_ID];
      return { ...cfg, channels };
    },
  },
  setup: setupAdapter,
  setupContract: defineChannelSetupContract({ fields: SETUP_FIELDS, adapter: setupAdapter }),
};
