import { defineChannelPluginEntry, isTrustedProxyAddress } from 'openclaw/plugin-sdk/core';
import { createReplyPrefixOptions } from 'openclaw/plugin-sdk/channel-outbound';
import { getPluginRuntimeGatewayRequestScope } from 'openclaw/plugin-sdk/plugin-runtime';
import { channelReadyPatch, channelStoppedPatch, createTransportActivityStatusPatch } from 'openclaw/plugin-sdk/gateway-runtime';
import { channelPlugin } from './channel.js';
import { registerCronHooks } from './cron.js';
import { registerDeviceRoute } from './gateway-route.js';
import { getServer } from './monitor.js';
import { setRuntime } from './runtime.js';
export { channelPlugin };
export const sdkHelpers = { replyPrefix: createReplyPrefixOptions, channelReadyPatch, channelStoppedPatch,
  transportActivityPatch: createTransportActivityStatusPatch };
export default defineChannelPluginEntry({ id: 'kubik', name: 'Kubik',
  description: 'Kubik voice desk device channel', plugin: channelPlugin,
  setRuntime: (runtime) => setRuntime(runtime, sdkHelpers),
  registerFull: (api) => {
    registerCronHooks(api);
    registerDeviceRoute(api, getServer, getPluginRuntimeGatewayRequestScope, { isTrusted: isTrustedProxyAddress,
      proxies: () => (api.runtime?.config?.current?.() ?? api.config)?.gateway?.trustedProxies ?? [] });
  } });
