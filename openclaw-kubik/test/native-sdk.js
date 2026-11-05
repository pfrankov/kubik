/** Controlled SDK-resolution latency for capability transaction/reconnect tests. */
export function nativeSdk(provider) {
  return {
    getRealtimeVoiceProvider: () => provider,
    async resolveConfiguredRealtimeVoiceProvider(request) {
      const { cfg, agentId, surface, autoRespondToAudio } = request;
      const providerConfig = provider.resolveConfig({ cfg, agentId, surface, autoRespondToAudio,
        rawConfig: request.providerConfigs.openai });
      if (!await provider.isConfigured({ cfg, agentId, providerConfig })) {
        throw new Error('Realtime voice provider "openai" is not configured');
      }
      return { provider, providerConfig, capabilities: provider.capabilities ?? {} };
    },
  };
}
