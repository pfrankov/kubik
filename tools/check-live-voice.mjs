#!/usr/bin/env node
// Default: offline SDK metadata. --live is an explicitly gated, bounded provider check.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFile } from 'node:fs/promises';

function options(args) {
  const result = { live: false, agent: 'main' };
  for (let at = 0; at < args.length; at++) {
    const name = args[at];
    if (name === '--live') { result.live = true; continue; }
    assert.ok(['--package', '--config', '--agent'].includes(name), 'Unknown check option');
    const value = args[++at];
    assert.ok(value && !value.startsWith('--'), 'Check option requires a value');
    result[name.slice(2)] = value;
  }
  assert.ok(result.live || !result.config, 'Offline metadata must not read credentials');
  if (result.live) assert.ok(result.config && process.env.KUBIK_TEST_LIVE === '1',
    'Paid check requires --config and KUBIK_TEST_LIVE=1; obtain explicit authorization first');
  return result;
}
const opts = options(process.argv.slice(2));
const require = createRequire(opts.package ?? new URL('../openclaw-kubik/package.json', import.meta.url));
const sdk = await import(require.resolve('openclaw/plugin-sdk/realtime-voice'));
async function readConfig(path) {
  const source = await readFile(path, 'utf8');
  try { return JSON.parse(source); }
  catch { throw new Error('Invalid OpenClaw config JSON'); }
}
const cfg = opts.live ? await readConfig(opts.config) : {};
const provider = sdk.getRealtimeVoiceProvider('openai', cfg);
assert.ok(provider, 'The installed OpenClaw SDK has no OpenAI voice provider');
assert.equal(typeof sdk.createRealtimeVoiceBridgeSession, 'function');
assert.equal(typeof provider.createBridge, 'function');

const format = { encoding: 'pcm16', sampleRateHz: 24000, channels: 1 };
const matches = (formats) => formats?.some((item) =>
  item.encoding === format.encoding && item.sampleRateHz === format.sampleRateHz && item.channels === 1);
const modes = ['gpt-realtime-2.1', 'gpt-live-1'].map((model) => {
  assert.ok(provider.models.includes(model), `The installed SDK does not list ${model}`);
  const capabilities = sdk.resolveRealtimeVoiceProviderCapabilities({
    provider, cfg, providerConfig: { model }, model, surface: 'gateway-relay',
  });
  assert.ok(matches(capabilities.inputAudioFormats), `${model} cannot accept device PCM`);
  assert.ok(matches(capabilities.outputAudioFormats), `${model} cannot produce device PCM`);
  return {
    model,
    pcm24kInput: true,
    pcm24kOutput: true,
    agentDelegation: capabilities.handlesAgentConsult ? 'native' :
      capabilities.supportsToolCalls ? 'function tool' : 'unsupported',
  };
});
assert.ok(modes.every((mode) => mode.agentDelegation !== 'unsupported'));
function failure(error) {
  // Provider errors may echo keys: report a category, never their raw message.
  const message = String(error?.message ?? '');
  if (/401|403|api.?key|auth/i.test(message)) return 'provider_auth';
  if (/429|quota|rate.limit/i.test(message)) return 'provider_limit';
  if (/404|model.*(?:not|unavailable)/i.test(message)) return 'provider_model';
  return 'provider_error';
}

async function closeBridge(bridge) {
  let timer;
  try { await Promise.race([Promise.resolve().then(() => bridge?.close({ disposition: 'detach' })),
    new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('provider_cleanup_timeout')), 16_000); })]); }
  finally { clearTimeout(timer); }
}

async function probe(model) {
  const providerConfig = provider.resolveConfig({ cfg, agentId: opts.agent, surface: 'gateway-relay',
    rawConfig: { model, voice: cfg.channels?.kubik?.voice?.voice ?? 'marin' }, autoRespondToAudio: model === 'gpt-live-1' });
  assert.ok(await provider.isConfigured({ cfg, agentId: opts.agent, providerConfig }), 'provider_auth_missing');
  const capabilities = sdk.resolveRealtimeVoiceProviderCapabilities({ provider, cfg, agentId: opts.agent,
    providerConfig, model, surface: 'gateway-relay' });
  let finish, reject, quiet, bytes = 0, firstAudioMs, requested = false, closing = false;
  const began = Date.now();
  const completion = new Promise((resolve, fail) => { finish = resolve; reject = fail; });
  completion.catch(() => {});
  const deadline = setTimeout(() => reject(new Error('provider_timeout')), 25_000);
  let bridge;
  try {
    bridge = sdk.createRealtimeVoiceBridgeSession({ cfg, agentId: opts.agent, provider, providerConfig, capabilities,
      audioFormat: format, autoRespondToAudio: model === 'gpt-live-1', interruptResponseOnInputAudio: false,
      instructions: 'This is a voice connectivity check. Say only the supplied short phrase, in Russian.',
      runAgentConsult: async () => ({ text: 'Готово.' }),
      audioSink: { isOpen: () => !closing, sendAudio(pcm) {
        if (!requested || !sdk.isRealtimeVoiceAudioAudible(pcm, format)) return;
        if (pcm.length % 2) { reject(new Error('provider_audio_format')); return; }
        firstAudioMs ??= Date.now() - began; bytes += pcm.length;
        clearTimeout(quiet); quiet = setTimeout(finish, 1000);
        if (bytes >= 4 * 24_000 * 2) finish();
      } },
      onResponseDone: ({ status }) => { if (status === 'completed' && bytes) finish(); },
      onError: (error) => reject(new Error(failure(error))),
      onClose: () => { if (!closing) reject(new Error('provider_closed')); },
      markStrategy: 'ignore',
    });
    await Promise.race([bridge.connect(), completion]);
    const connectMs = Date.now() - began;
    requested = true;
    bridge.sendUserMessage(sdk.buildRealtimeVoiceSpeakExactMessage({ text: 'Готово.', surfaceLabel: 'Kubik check' }));
    await completion;
    assert.ok(bytes > 0, 'provider_empty_audio');
    return { model, connected: true, audiblePcmBytes: bytes, connectMs, firstAudioMs };
  } finally {
    closing = true; clearTimeout(deadline); clearTimeout(quiet);
    await closeBridge(bridge);
  }
}

if (opts.live) {
  for (const { model } of modes) {
    try { console.log(JSON.stringify(await probe(model))); }
    catch (error) {
      const category = /^provider_[a-z_]+$/.test(error.message) ? error.message : failure(error);
      console.log(JSON.stringify({ model, passed: false, category })); process.exitCode = 1;
    }
  }
} else console.log(JSON.stringify({ scope: 'offline SDK metadata; not account access or latency', modes }, null, 2));
