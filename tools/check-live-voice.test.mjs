import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const script = fileURLToPath(new URL('./check-live-voice.mjs', import.meta.url));

async function fixture(t) {
  const dir = await mkdtemp(join(tmpdir(), 'kubik-live-metadata-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const marker = join(dir, 'network-attempts');
  const preload = join(dir, 'no-network.mjs');
  await writeFile(preload, `
    import fs from 'node:fs';import net from 'node:net';import tls from 'node:tls';
    import http from 'node:http';import https from 'node:https';import {syncBuiltinESMExports} from 'node:module';
    const blocked=()=>{fs.appendFileSync(process.env.KUBIK_CHECK_NETWORK_MARKER,'blocked\\n');throw Error('Network prohibited in this test');};
    globalThis.fetch=blocked;net.Socket.prototype.connect=blocked;net.connect=blocked;net.createConnection=blocked;
    tls.connect=blocked;http.request=blocked;http.get=blocked;https.request=blocked;https.get=blocked;syncBuiltinESMExports();
  `);
  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !/API_KEY|TOKEN|SECRET|PASSWORD|KUBIK_TEST_LIVE/i.test(key)));
  Object.assign(env, { OPENCLAW_STATE_DIR: join(dir, 'state'), OPENCLAW_CONFIG_PATH: join(dir, 'no-config.json'),
    KUBIK_CHECK_NETWORK_MARKER: marker, KUBIK_CHECK_RESOLVER_MARKER: join(dir, 'resolved.jsonl') });
  return { dir, env, marker, run: (args, extra = {}) => spawnSync(process.execPath, ['--import', preload, script, ...args],
    { env: { ...env, ...extra }, encoding: 'utf8', timeout: 30_000 }) };
}

async function noNetwork(f) {
  await assert.rejects(readFile(f.marker), { code: 'ENOENT' });
}

test('actual pinned SDK advertises both models offline without credentials, config or network', async (t) => {
  const f = await fixture(t);const result = f.run([]);
  assert.equal(result.status, 0, result.stderr);
  const report = JSON.parse(result.stdout);
  assert.match(report.scope, /advertised provider metadata/);
  assert.match(report.scope, /not configured model capabilities, account access or latency/);
  assert.deepEqual(report.modes.map((mode) => mode.model), ['gpt-realtime-2.1', 'gpt-live-1']);
  assert.ok(report.modes.every((mode) => mode.pcm24kInput && mode.pcm24kOutput && mode.agentDelegation !== 'unsupported'));
  await noNetwork(f);
});

async function mockSdk(f) {
  const parent = join(f.dir, 'consumer');const module = join(parent, 'node_modules/openclaw');
  await mkdir(module, { recursive: true });
  const packagePath = join(parent, 'package.json');await writeFile(packagePath, '{}');
  await writeFile(join(module, 'package.json'), JSON.stringify({ name: 'openclaw', type: 'module',
    exports: { './plugin-sdk/realtime-voice': './voice.mjs' } }));
  await writeFile(join(module, 'voice.mjs'), `
    import assert from 'node:assert/strict';import {appendFileSync} from 'node:fs';
    const format={encoding:'pcm16',sampleRateHz:24000,channels:1};
    const capabilities={inputAudioFormats:[format],outputAudioFormats:[format],supportsToolCalls:true};
    const forbidden=()=>{throw Error('Offline metadata must not resolve credentials or create a provider');};
    const provider={models:['gpt-realtime-2.1','gpt-live-1'],capabilities,resolveConfig:forbidden,isConfigured:forbidden,createBridge:forbidden};
    export const getRealtimeVoiceProvider=()=>provider;
    export async function resolveConfiguredRealtimeVoiceProvider(request){
      assert.equal(request.configuredProviderId,'openai');assert.deepEqual(request.providers,[provider]);
      assert.equal(request.surface,'gateway-relay');assert.equal(request.agentId,'test-agent');
      const cfg=request.providerConfigs.openai;assert.equal(cfg.voice,'marin');
      assert.equal(request.autoRespondToAudio,cfg.model==='gpt-live-1');
      appendFileSync(process.env.KUBIK_CHECK_RESOLVER_MARKER,JSON.stringify({model:cfg.model})+'\\n');
      return {provider,providerConfig:cfg,capabilities};
    }
    export function createRealtimeVoiceBridgeSession(options){
      assert.equal(options.provider,provider);assert.equal(options.capabilities,capabilities);
      return {connect:async()=>{},sendUserMessage(){queueMicrotask(()=>{options.audioSink.sendAudio(Buffer.alloc(1920,4));options.onResponseDone({status:'completed'});});},close:async()=>{}};
    }
    export const isRealtimeVoiceAudioAudible=()=>true;
    export const buildRealtimeVoiceSpeakExactMessage=({text})=>text;
  `);
  return packagePath;
}

test('offline mode never invokes configuration/auth/bridge methods and rejects credential-file input', async (t) => {
  const f = await fixture(t);const packagePath = await mockSdk(f);
  const result = f.run(['--package', packagePath]);assert.equal(result.status, 0, result.stderr);
  await assert.rejects(readFile(f.env.KUBIK_CHECK_RESOLVER_MARKER), { code: 'ENOENT' });
  const rejected = f.run(['--package', packagePath, '--config', join(f.dir, 'must-not-read.json')]);
  assert.notEqual(rejected.status, 0);assert.match(rejected.stderr, /Offline metadata must not read credentials/);
  assert.doesNotMatch(rejected.stderr, /ENOENT/);await noNetwork(f);
});

test('live guard fails before SDK/config access unless both explicit gates are present', async (t) => {
  const f = await fixture(t);
  for (const [args, env] of [[['--live'], {}], [['--live', '--config', 'must-not-read'], {}],
    [['--live'], { KUBIK_TEST_LIVE: '1' }]]) {
    const result = f.run([...args, '--package', join(f.dir, 'missing-sdk/package.json')], env);
    assert.notEqual(result.status, 0);assert.match(result.stderr, /Paid check requires --config and KUBIK_TEST_LIVE=1/);
    assert.doesNotMatch(result.stderr, /ERR_MODULE_NOT_FOUND|ENOENT/);
  }
  await noNetwork(f);
});

test('authorized mock-only live mode uses the public configured-provider resolver for both models', async (t) => {
  const f = await fixture(t);const packagePath = await mockSdk(f);const cfg = join(f.dir, 'mock-config.json');
  await writeFile(cfg, JSON.stringify({ channels: { kubik: { voice: { voice: 'marin' } } }, private: 'never-log-this' }));
  const result = f.run(['--package', packagePath, '--live', '--config', cfg, '--agent', 'test-agent'], { KUBIK_TEST_LIVE: '1' });
  assert.equal(result.status, 0, result.stderr);assert.equal(result.stdout.includes('never-log-this'), false);
  const reports = result.stdout.trim().split('\n').map((line) => JSON.parse(line));
  assert.deepEqual(reports.map((report) => report.model), ['gpt-realtime-2.1', 'gpt-live-1']);
  assert.ok(reports.every((report) => report.connected && report.audiblePcmBytes === 1920));
  const calls = (await readFile(f.env.KUBIK_CHECK_RESOLVER_MARKER, 'utf8')).trim().split('\n').map((line) => JSON.parse(line));
  assert.deepEqual(calls.map((call) => call.model), ['gpt-realtime-2.1', 'gpt-live-1']);await noNetwork(f);
});
