import assert from 'node:assert/strict';
import test, { after } from 'node:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const stateDir = mkdtempSync(join(tmpdir(), 'kubik-agent-control-'));
after(() => rmSync(stateDir, { recursive: true, force: true }));
import { createAgentControl } from '../src/agent-control.js';

const device = { id: 'kubik-b6c634', name: 'Стол' };

function modelFixture({ allow = ['openrouter/meta-llama/llama-3', 'openai/gpt-5.2'], applySelection = true,
  blockAfterPersist = false } = {}) {
  const rows = [
    { provider: 'openrouter', id: 'meta-llama/llama-3', name: 'Llama 3' },
    { provider: 'openai', id: 'gpt-5.2', name: 'GPT 5.2' },
    { provider: 'openai', id: 'blocked', name: 'Not allowed' },
  ];
  const entries = new Map();
  const seen = { catalog: [], records: [], commands: [] };
  const core = {
    state: { resolveStateDir: () => stateDir },
    channel: {
      routing: { resolveAgentRoute: ({ peer }) => ({ agentId: 'main', sessionKey: `agent:main:kubik:direct:${peer.id}` }) },
      session: {
        resolveStorePath: () => '/sessions/store.sqlite',
        readSessionUpdatedAt: () => undefined,
        recordInboundSession: async ({ sessionKey, ...params }) => {
          seen.records.push(params);
          if (!entries.has(sessionKey)) entries.set(sessionKey, { sessionKey });
        },
      },
      reply: {
        finalizeInboundContext: (ctx) => ctx,
        resolveEnvelopeFormatOptions: () => ({}),
        formatAgentEnvelope: ({ channel, from, body }) => `[${channel} ${from}] ${body}`,
        dispatchReplyWithBufferedBlockDispatcher: async ({ ctx, dispatcherOptions, replyOptions }) => {
          seen.commands.push(ctx);
          const match = /^\/model ([^\s]+) -s$/.exec(ctx.CommandBody);
          if (match && applySelection) {
            const slash = match[1].indexOf('/');
            entries.set(ctx.SessionKey, { ...entries.get(ctx.SessionKey), providerOverride: match[1].slice(0, slash),
              modelOverride: match[1].slice(slash + 1), modelOverrideSource: 'user' });
          }
          if (blockAfterPersist) {
            seen.signal = replyOptions.abortSignal;
            return new Promise(() => {});
          }
          await dispatcherOptions.deliver({ text: 'Выбрана модель.' }, { kind: 'command' });
        },
      },
    },
    agent: { session: {
      getSessionEntry: ({ sessionKey }) => entries.get(sessionKey),
    } },
  };
  const modelRuntime = {
    loadPreparedModelCatalog: async (params) => { seen.catalog.push(params); return rows; },
    resolveAllowedModelRef: ({ raw, cfg }) => allow.includes(raw)
      ? { ref: { provider: raw.slice(0, raw.indexOf('/')), model: raw.slice(raw.indexOf('/') + 1) } }
      : { error: 'not-allowed' },
    resolveDefaultModelForAgent: () => ({ provider: 'openai', model: 'gpt-5.2' }),
  };
  const sdk = { replyPrefix: () => ({ onModelSelected: () => {} }) };
  return { core, sdk, modelRuntime, seen, entries };
}

test('per-device model picker uses the prepared allowlisted catalog and persists only on its session', async () => {
  const fixture = modelFixture();
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {}, account: { accountId: 'default', voice: { provider: 'openclaw' } },
    modelRuntime: fixture.modelRuntime });
  const options = await control.options(device);
  assert.deepEqual(options.models.map((model) => model.id), ['openrouter/meta-llama/llama-3', 'openai/gpt-5.2']);
  assert.equal(options.model, 'openai/gpt-5.2');
  assert.deepEqual(fixture.seen.catalog[0], { config: { session: { dmScope: 'per-account-channel-peer' } }, agentId: 'main',
    readOnly: true, providerDiscoveryProviderIds: [], scopedLiveProviderDiscovery: false });

  await control.selectModel(device, 'openrouter/meta-llama/llama-3');
  assert.equal(fixture.seen.records.length, 1, 'first authenticated model action creates the normal session metadata row');
  assert.equal(fixture.seen.commands.length, 1);
  assert.equal(fixture.seen.commands[0].CommandBody, '/model openrouter/meta-llama/llama-3 -s');
  assert.equal(fixture.seen.commands[0].CommandAuthorized, true);
  assert.equal(fixture.seen.commands[0].CommandSource, 'native');
  assert.equal(fixture.seen.commands[0].SessionKey, 'agent:main:kubik:direct:kubik-b6c634');
  assert.equal(fixture.seen.commands[0].CommandTargetSessionKey, fixture.seen.commands[0].SessionKey);
  assert.equal(fixture.seen.commands[0].SenderId, device.id);
  assert.equal(fixture.seen.commands[0].NativeChannelId, device.id);
  const updated = await control.options(device);
  assert.equal(updated.model, 'openrouter/meta-llama/llama-3', 'model names may contain slashes after the provider prefix');
  assert.equal(fixture.seen.records.length, 1, 'existing session metadata is reused');
  assert.ok(fixture.seen.catalog.every((call) => call.providerDiscoveryProviderIds.length === 0));
});

test('model options follow OpenClaw stored-field precedence and ignore runtime fields for default/auto source', async () => {
  const cases = [
    { entry: { modelProvider: 'openai', model: 'gpt-6-luna' }, expected: 'openai/gpt-6-luna' },
    { entry: { modelProvider: 'openai', model: 'gpt-6-luna', providerOverride: 'openrouter',
      modelOverride: 'meta-llama/llama-3', modelOverrideSource: 'user' }, expected: 'openrouter/meta-llama/llama-3' },
    { entry: { modelProvider: 'openai', model: 'gpt-6-luna', providerOverride: 'openrouter',
      modelOverride: 'meta-llama/llama-3', modelOverrideSource: 'default' }, expected: 'openai/gpt-5.2' },
    { entry: { modelProvider: 'openai', model: 'gpt-6-luna', providerOverride: 'openrouter',
      modelOverride: 'meta-llama/llama-3', modelOverrideSource: 'auto' }, expected: 'openai/gpt-5.2' },
  ];
  for (const { entry, expected } of cases) {
    const fixture = modelFixture();
    fixture.entries.set('agent:main:kubik:direct:kubik-b6c634', entry);
    const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {},
      account: { accountId: 'default', voice: { provider: 'openclaw' } }, modelRuntime: fixture.modelRuntime });
    assert.equal((await control.options(device)).model, expected);
  }
});

test('model-selection persistence readback uses the same official stored-field resolver as options', async () => {
  const fixture = modelFixture();
  const sessionKey = 'agent:main:kubik:direct:kubik-b6c634';
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {},
    account: { accountId: 'default', voice: { provider: 'openclaw' } }, modelRuntime: fixture.modelRuntime,
    commandDispatch: async ({ modelId, isPersisted }) => {
      const slash = modelId.indexOf('/');
      fixture.entries.set(sessionKey, { modelProvider: modelId.slice(0, slash), model: modelId.slice(slash + 1) });
      assert.equal(isPersisted(), true);
    },
  });
  await control.selectModel(device, 'openrouter/meta-llama/llama-3');
  assert.equal((await control.options(device)).model, 'openrouter/meta-llama/llama-3');
});

test('model selection rejects disallowed and malformed ids before creating session state', async () => {
  const fixture = modelFixture({ allow: ['openai/gpt-5.2'] });
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {}, account: { accountId: 'default', voice: { provider: 'openclaw' } },
    modelRuntime: fixture.modelRuntime });
  await assert.rejects(control.selectModel(device, 'openrouter/meta-llama/llama-3'), { code: 'invalid_model' });
  await assert.rejects(control.selectModel(device, 'provider/model\n'), { code: 'invalid_model' });
  await assert.rejects(control.selectModel(device, 'x'.repeat(161)), { code: 'invalid_model' });
  assert.equal(fixture.seen.records.length, 0);
  assert.equal(fixture.seen.commands.length, 0);
});

test('model selection is not acknowledged when native command did not persist the session model', async () => {
  const fixture = modelFixture({ applySelection: false });
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {},
    account: { accountId: 'default', voice: { provider: 'openclaw' } }, modelRuntime: fixture.modelRuntime });
  await assert.rejects(control.selectModel(device, 'openrouter/meta-llama/llama-3'), { code: 'unavailable' });
  assert.equal(fixture.seen.commands[0].CommandBody, '/model openrouter/meta-llama/llama-3 -s');
  assert.equal((await control.options(device)).model, 'openai/gpt-5.2');
});

test('model selection acknowledges exact-session persistence without waiting for full reply dispatch', async () => {
  const fixture = modelFixture({ blockAfterPersist: true });
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {},
    account: { accountId: 'default', voice: { provider: 'openclaw' } }, modelRuntime: fixture.modelRuntime });
  const started = Date.now();
  await control.selectModel(device, 'openrouter/meta-llama/llama-3');
  assert.ok(Date.now() - started < 1_000, 'the ACK follows session persistence instead of hanging reply dispatch');
  assert.equal(fixture.seen.signal?.aborted, true, 'the outstanding reply dispatch is cancelled after readback');
  assert.equal((await control.options(device)).model, 'openrouter/meta-llama/llama-3');
});

test('model selection timeout aborts a dispatch that never persists', async () => {
  const fixture = modelFixture({ applySelection: false, blockAfterPersist: true });
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {}, selectionTimeoutMs: 30,
    account: { accountId: 'default', voice: { provider: 'openclaw' } }, modelRuntime: fixture.modelRuntime });
  await assert.rejects(control.selectModel(device, 'openrouter/meta-llama/llama-3'), { code: 'timeout' });
  assert.equal(fixture.seen.signal?.aborted, true);
  assert.equal((await control.options(device)).model, 'openai/gpt-5.2');
});

test('custom OpenAI HTTP voice reports only configured provider metadata, never the key', async () => {
  const fixture = modelFixture();
  const control = createAgentControl({ core: fixture.core, sdk: fixture.sdk, cfg: {}, account: { accountId: 'default', voice: {
    provider: 'openai-http', apiKey: 'sk-test-secret', transcribeModel: 'whisper-1', ttsModel: 'gpt-4o-mini-tts',
  } }, modelRuntime: fixture.modelRuntime });
  const options = await control.options(device);
  assert.deepEqual(options.stt, { available: true, provider: 'openai', model: 'whisper-1' });
  assert.deepEqual(options.tts, { available: true, provider: 'openai', model: 'gpt-4o-mini-tts' });
  assert.equal(JSON.stringify(options).includes('sk-test-secret'), false);
});
