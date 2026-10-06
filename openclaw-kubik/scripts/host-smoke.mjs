// Packs the plugin, loads the tarball contents against the real pinned OpenClaw SDK, registers the channel
// through the SDK entry helper and runs one complete voice turn and one proactive notification on the
// packed code (fake OpenClaw core, fake voice engine, real WebSocket device client over the LAN TLS listener).
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, mkdtemp, readdir, readFile, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { config, connectDevice, DEFAULT_DEVICE_KEY, DEVICE, fakeEngine, fakePairing, installRuntime } from '../test/helpers.js';

const root = resolve(fileURLToPath(new URL('../', import.meta.url)));
process.chdir(root);
try { import.meta.resolve('openclaw/plugin-sdk/core'); } catch { throw new Error('Install the pinned host first: npm install'); }
const hostVersion = JSON.parse(await readFile(join(root, 'node_modules/openclaw/package.json'), 'utf8')).version;
await mkdir('.tmp', { recursive: true });
const dir = await mkdtemp(join(root, '.tmp', 'host-'));
try {
  const npm = process.env.npm_execpath;
  const args = ['pack', '--ignore-scripts', '--json', '--pack-destination', dir];
  const [pack] = JSON.parse(npm ? execFileSync(process.execPath, [npm, ...args], { encoding: 'utf8' }) : execFileSync('npm', args, { encoding: 'utf8' }));
  execFileSync('tar', ['-xzf', join(dir, pack.filename), '-C', dir]);
  const base = join(dir, 'package');
  const files = pack.files.map((f) => f.path).sort();
  assert.ok(files.includes('openclaw.plugin.json') && files.includes('src/index.js') && files.includes('README.md') && files.includes('LICENSE'), files.join(', '));
  assert.ok(!files.some((f) => f.startsWith('test/') || f.startsWith('scripts/')), 'tests/scripts are not published');
  // Loading the tarball, rather than the checkout, catches missing published files and bad SDK imports.
  const walk = async (d) => (await readdir(d, { withFileTypes: true })).flatMap((e) => (e.isDirectory() ? [] : [join(d, e.name)]));
  for (const file of [...await walk(join(base, 'src')), ...await walk(join(base, 'src/engines'))]) await import(pathToFileURL(file));
  const entry = await import(pathToFileURL(join(base, 'src/index.js')));
  const setup = await import(pathToFileURL(join(base, 'src/setup-entry.js')));
  const { monitorAccount, transportStatus } = await import(pathToFileURL(join(base, 'src/monitor.js')));
  const { resolveAccount } = await import(pathToFileURL(join(base, 'src/config.js')));

  const cfg = config();
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry] });
  const { core, seen } = installRuntime({ payloads: [{ text: '[[happy]] Привет! **Я** Кубик 🙂' }], pairing: pairing.api });
  let registered;
  const routes = [];
  await entry.default.register({ runtime: core, config: cfg, registrationMode: 'full', registerHttpRoute: (route) => routes.push(route),
    registerChannel: (value) => { registered = value.plugin ?? value; }, logger: { info() {}, warn() {}, error() {}, debug() {} } });
  assert.equal(registered.id, 'kubik');
  assert.deepEqual(routes.map((r) => [r.path, r.auth, typeof r.handleUpgrade]), [['/kubik/v1', 'plugin', 'function']]);
  assert.equal(setup.default.plugin?.id ?? setup.setupPlugin.id, 'kubik');
  const metadata = JSON.parse(await readFile(join(base, 'openclaw.plugin.json'), 'utf8'));
  assert.deepEqual(registered.configSchema.schema, metadata.channelConfigs.kubik.schema);
  assert.equal(registered.outbound.deliveryMode, 'gateway');
  assert.equal(registered.capabilities.blockStreaming, true);

  // Real SDK helpers (installed by register → setRuntime) drive status patches and reply prefixes.
  const ready = entry.sdkHelpers.channelReadyPatch({ listen: 'x' });
  assert.equal(ready.connected, true);
  assert.equal(ready.listen, 'x');
  const prefix = entry.sdkHelpers.replyPrefix({ cfg, agentId: 'main', channel: 'kubik', accountId: 'default' });
  assert.equal(typeof prefix, 'object');

  const account = resolveAccount(cfg, 'default', { env: {} });
  account.listen = { ...account.listen, port: 0 };
  const statuses = [];
  const engine = fakeEngine({ transcripts: ['Как дела?'] });
  // The packed monitor resolves its own runtime (set by register), so this also checks the entry wiring.
  const server = await monitorAccount({ account, cfg, setStatus: (patch) => statuses.push(patch), log: { info() {}, warn() {} } },
    { serverOptions: { engineFactory: () => engine },
      lan: { stateDir: join(dir, 'state'), host: '127.0.0.1', discoveryHost: '127.0.0.1', discoveryPort: 0 } });
  try {
    const { lan } = transportStatus();
    assert.match(lan.spki, /^[0-9a-f]{64}$/);
    const device = await connectDevice(`wss://127.0.0.1:${lan.port}/kubik/v1`, { bind: 'seen' });
    await device.waitFor((e) => e.t === 'welcome');
    device.send({ t: 'ptt', on: true, turn: 1 });
    device.sendAudio(1, Buffer.alloc(48 * 400, 1));
    device.send({ t: 'ptt', on: false, turn: 1 });
    await device.waitFor((e) => e.t === 'speak_end');
    assert.equal(seen.contexts[0].BodyForAgent, 'Как дела?');
    assert.equal(seen.contexts[0].CommandAuthorized, false);
    assert.deepEqual(engine.calls.speak, ['Привет! Я Кубик.']);
    assert.ok(device.events.some((e) => e.t === 'emotion' && e.e === 'happy'));
    assert.ok(statuses.some((s) => s.lifecycle === 'ready'));
    const sent = await registered.outbound.sendText({ cfg, to: `kubik:${DEVICE}`, text: 'Пора пить воду.', accountId: 'default' });
    assert.equal(sent.channel, 'kubik');
    assert.ok(device.events.some((e) => e.t === 'speak' && e.kind === 'notify'));
    device.close();
  } finally { await server.stop(); }
  console.log(`Packed plugin (${files.length} files) with real OpenClaw SDK ${hostVersion}: registration, schema/manifest sync, SDK status/reply-prefix helpers, route registration, LAN TLS + v5 SPKI-bound device auth, voice turn → agent dispatch → speech, notify outbound passed`);
} finally { await rm(dir, { recursive: true, force: true }); }
