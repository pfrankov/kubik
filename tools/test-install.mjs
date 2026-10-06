#!/usr/bin/env node
// End-to-end test of the documented installation on a clean OpenClaw (target: under 3 minutes, no hidden steps).
//
//   node tools/test-install.mjs [--openclaw 2026.9.7] [--kit dist/kubik-kit-tess.zip] [--keep]
//
// Installs the OpenClaw version named by --openclaw into a temp prefix with an isolated HOME/state/config and a
// random Gateway port, then types the commands from docs/KIT.ru.md (the kit's README.md) and lets a simulated
// device (tools/test-install/device.mjs) discover, pin, pair, talk, reconnect and fail like the firmware would.
// STT/TTS/LLM are tools/mock-openai. Needs openssl, UDP 18790 free (the plugin's fixed discovery port) and network
// access to npm the first time. Exits 0 when every step passes; the temp directory and Gateway are always removed.
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { randomBytes } from 'node:crypto';
import { startMockServer, createSynth, STT_PHRASES } from './mock-openai/server.mjs';
import { discoverServer } from './usb-bridge/lan-discovery.mjs';
import { loadInstallDocs, documented } from './test-install/docs.mjs';
import { newDeviceKeyPem, PinMismatch, SimDevice } from './test-install/device.mjs';
import { createCa, startForwarder } from './test-install/proxy.mjs';
import { assertUdpFree, freePort, realStateFingerprint, Sandbox, satisfiesRange, tcpFree } from './test-install/sandbox.mjs';

const execFileAsync = promisify(execFile);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const option = (name, fallback) => { const i = argv.indexOf(`--${name}`); return i >= 0 && argv[i + 1] ? argv[i + 1] : fallback; };
const started = performance.now();
const seconds = () => ((performance.now() - started) / 1000).toFixed(1);
const say = (message) => console.log(`[${seconds().padStart(5)}s] ${message}`);
const sleep = (ms) => new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
const CLIENT_IP = '198.51.100.23'; // the client address the test forwarder reports to the Gateway
const ctx = { warnings: [], skipped: [] };

async function preflight() {
  await execFileAsync('openssl', ['version']).catch(() => { throw new Error('openssl is required (creates the test CA)'); });
  await assertUdpFree(18790);
  ctx.lanPortBusy = !(await tcpFree(18790));
  ctx.pluginManifest = JSON.parse(await readFile(join(ROOT, 'openclaw-kubik/package.json'), 'utf8'));
  ctx.openclawVersion = option('openclaw', '2026.9.7');
  const range = ctx.pluginManifest.peerDependencies.openclaw;
  assert.ok(satisfiesRange(range, ctx.openclawVersion), `OpenClaw ${ctx.openclawVersion} is outside the plugin's declared range ${range}`);
  ctx.dir = await mkdtemp(join(tmpdir(), 'kubik-install-test-'));
  ctx.box = new Sandbox(ctx.dir, await freePort());
  ctx.realBefore = await realStateFingerprint();
  ctx.docs = await loadInstallDocs(ROOT);
  ctx.deviceId = `kubik-${randomBytes(3).toString('hex')}`;
  ctx.keyPem = newDeviceKeyPem();
}

async function archiveFromKit(kit, tgz) {
  const { stdout } = await execFileAsync('unzip', ['-p', resolve(kit), 'openclaw-kubik-*.tgz'], { encoding: 'buffer', maxBuffer: 67_108_864 });
  assert.ok(stdout.length > 1000, `${kit} has no openclaw-kubik-*.tgz`);
  await writeFile(tgz, stdout);
  say(`plugin archive taken from ${kit}`);
}

async function archiveFromPack(tgz) {
  const { stdout } = await execFileAsync('npm', ['pack', '--ignore-scripts', '--json', '--pack-destination', ctx.dir], { cwd: join(ROOT, 'openclaw-kubik') });
  await execFileAsync('mv', [join(ctx.dir, JSON.parse(stdout)[0].filename), tgz]);
  say('plugin archive from `npm pack` (the same command tools/package-kit.py runs)');
}

async function prepareKit() {
  const kit = option('kit');
  ctx.tgz = join(ctx.dir, `openclaw-kubik-${ctx.pluginManifest.version}.tgz`); // the kit ships this name; the docs install it by path
  await (kit ? archiveFromKit(kit, ctx.tgz) : archiveFromPack(ctx.tgz));
}

async function cleanOpenClaw() {
  const version = await ctx.box.installOpenClaw(ctx.openclawVersion);
  assert.equal(version, ctx.openclawVersion);
  const file = (await ctx.box.mustRun('openclaw config file')).trim();
  assert.ok(file.includes(basename(ctx.dir)), `openclaw would use ${file}, outside the sandbox ${ctx.dir}`);
  ctx.configFile = file;
  assert.equal(ctx.box.gatewayPort === 18789, false);
  process.env.MOCK_DUMP_CHAT = join(ctx.dir, 'last-chat.json'); // the mock writes the agent's last LLM request here
  ctx.mock = await startMockServer({ port: 0, tts: 'say', log: () => {} });
  await ctx.box.mustRun(`openclaw onboard --non-interactive --accept-risk --mode local --no-install-daemon --auth-choice custom-api-key `
    + `--custom-base-url ${ctx.mock.url} --custom-model-id mock-llm --custom-api-key sk-mock --custom-compatibility openai `
    + `--gateway-port ${ctx.box.gatewayPort} --gateway-bind loopback --gateway-auth token --skip-channels --skip-skills --skip-search --skip-health --skip-ui`);
  // The customer's existing voice setup (KIT.ru.md: STT and TTS already configured on the host), pointed at the mock.
  await ctx.box.mustRun(`printf 'OPENAI_API_KEY=sk-mock\\n' > "$OPENCLAW_STATE_DIR/.env"`);
  const stt = JSON.stringify([{ provider: 'openai', model: 'gpt-4o-transcribe', baseUrl: ctx.mock.url, capabilities: ['audio'] }]);
  const tts = JSON.stringify({ provider: 'openai', providers: { openai: { apiKey: 'sk-mock', baseUrl: ctx.mock.url, model: 'gpt-4o-mini-tts', responseFormat: 'wav' } } });
  await ctx.box.mustRun(`openclaw config set tools.media.models '${stt}' --strict-json`);
  // OpenClaw's SSRF guard blocks provider calls to loopback unless the provider opts in (a real STT/TTS host needs none).
  const provider = JSON.stringify({ baseUrl: ctx.mock.url, apiKey: 'sk-mock', models: [], request: { allowPrivateNetwork: true } });
  await ctx.box.mustRun(`openclaw config set models.providers.openai '${provider}' --strict-json`);
  await ctx.box.mustRun(`openclaw config set tts '${tts}' --strict-json`);
  // Documented for reverse proxies (deploy/cloudflared): the Gateway trusts the forwarder for client addresses.
  await ctx.box.mustRun(`openclaw config set gateway.trustedProxies '["127.0.0.1"]' --strict-json`);
  const { plugins } = JSON.parse(await ctx.box.mustRun('openclaw plugins list --json'));
  assert.ok(!plugins.some((plugin) => plugin.id === 'kubik'), 'the clean OpenClaw must not know the Kubik plugin yet');
}

async function checkNativeConfigGuard() {
  const before = await readFile(ctx.configFile, 'utf8');
  const voice = { provider: 'openclaw', mode: 'realtime', liveTranscription: true, apiKey: null, apiKeyFile: null };
  const patch = join(ctx.dir, 'native-voice-check.json');
  const writePatch = () => writeFile(patch, JSON.stringify({ channels: { kubik: { voice } } }));
  const command = 'openclaw config patch --file native-voice-check.json --dry-run --json';
  await writePatch();
  const invalid = await ctx.box.run(command);
  assert.equal(invalid.ok, false, 'installed manifest accepted incompatible native voice');
  assert.match(invalid.output, /channels\.kubik\.voice.*invalid config/);
  voice.liveTranscription = false;
  await writePatch();
  const valid = JSON.parse(await ctx.box.mustRun(command));
  assert.equal(valid.ok, true, 'installed manifest rejected coherent native voice');
  assert.equal(await readFile(ctx.configFile, 'utf8'), before, 'dry-run modified the working channel');
}

function readLanStatus(status, publicPeers = false) {
  const suffix = publicPeers ? ' public' : '';
  const pattern = new RegExp(`mode:lan (kubik://[\\d.]+:\\d+(?:, kubik://[\\d.]+:\\d+)*)${suffix} spki:([0-9a-f]{16}); route /kubik/v1`);
  const match = pattern.exec(status);
  assert.ok(match, `status lacks copyable Kubik listener addresses/SPKI/route:\n${status}`);
  const addresses = match[1].split(', ').map((address) => new URL(address));
  const port = addresses[0].port;
  assert.ok(addresses.every((address) => address.port === port), 'every copyable address must declare the listener port');
  return { addresses, port, spkiPrefix: match[2] };
}

async function installFollowingDocs() {
  const archive = `openclaw-kubik-${ctx.pluginManifest.version}.tgz`;
  // The manual gives the same command for macOS/Linux (./file) and Windows (.\\file); the test runs the first.
  assert.ok(ctx.docs.some((command) => command.startsWith(`openclaw plugins install .\\${archive} `)), 'the docs lack the Windows plugin install command');
  const install = documented(ctx.docs, `openclaw plugins install ./${archive} `, { [`./${archive}`]: ctx.tgz });
  assert.match(await ctx.box.mustRun(install), /Installed plugin: kubik/);
  assert.match(await ctx.box.mustRun(documented(ctx.docs, 'openclaw channels add --channel kubik --enable')), /Added Kubik account "default"/);
  assert.equal(documented(ctx.docs, 'openclaw gateway run'), 'openclaw gateway run');
  if (ctx.lanPortBusy) {
    const port = ctx.configuredPort = await freePort();
    await ctx.box.mustRun(`openclaw config set channels.kubik.listen.port ${port}`);
    ctx.warnings.push(`TCP 18790 is taken by another Kubik gateway on this machine, so channels.kubik.listen.port=${port} was set (discovery still answers on UDP 18790)`);
  }
  ctx.statusBefore = await ctx.box.startGateway();
  await checkNativeConfigGuard();
  const probe = await ctx.box.mustRun(documented(ctx.docs, 'openclaw channels status --probe'));
  assert.match(probe, /; route \/kubik\/v1, works/);
  const listener = readLanStatus(probe);
  ctx.lanPort = listener.port; ctx.spkiPrefix = listener.spkiPrefix;
  assert.equal(ctx.lanPort, ctx.lanPortBusy ? String(ctx.configuredPort) : '18790');
}

async function discoverLan() {
  let url;
  try { url = await discoverServer({ address: '255.255.255.255' }); ctx.discovery = 'broadcast'; }
  catch { url = await discoverServer({ address: '127.0.0.1' }); ctx.discovery = 'loopback unicast (broadcast unanswered on this host)'; }
  assert.match(url, new RegExp(`^wss://[\\d.]+:${ctx.lanPort}/kubik/v1$`));
  ctx.lanUrl = url;
  say(`discovery via ${ctx.discovery}: ${url}`);
}

const newDevice = (pin = null, fw = undefined) => new SimDevice({ id: ctx.deviceId, keyPem: ctx.keyPem, pin, fw });

/** Connects an unknown device, approves its code through the documented CLI and reports how it got its welcome. */
async function pairThroughCli(device) {
  await device.connect(ctx.lanUrl);
  assert.ok(device.observedSpki.startsWith(ctx.spkiPrefix), 'the TLS key the device pinned is not the SPKI shown by status --probe');
  const pair = await device.outcome(15_000);
  assert.equal(pair.t, 'pair', `expected a pairing code, got ${pair.t}`);
  assert.match(pair.code, /^[A-Z0-9]{8}$/);
  const listed = await ctx.box.mustRun(documented(ctx.docs, 'openclaw pairing list kubik'));
  assert.ok(listed.includes(pair.code) && listed.includes(device.id), `pairing list does not show ${pair.code}:\n${listed}`);
  const approvedAt = performance.now();
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw pairing approve kubik', { ABCD2345: pair.code }));
  const event = await device.waitFor((e) => e.t === 'welcome' || e.t === '__closed__', 15_000);
  return { sameSocket: event.t === 'welcome', approvedAt };
}

/** Reconnects the way the firmware does after a drop, until the approved key is welcomed. */
async function reconnectUntilWelcome(device) {
  for (let attempt = 1; attempt <= 5; attempt++) {
    await sleep(1000);
    await device.connect(ctx.lanUrl, {}).catch(() => {});
    const event = await device.outcome(15_000).catch(() => ({ t: 'timeout' }));
    assert.notEqual(event.t, 'pair', 'an approved key must not get a new pairing code');
    if (event.t === 'welcome') return attempt;
  }
  throw new Error('the approved device was not welcomed after 5 reconnects');
}

const channelStarts = async () => ((await readFile(ctx.box.gatewayLog, 'utf8')).match(/\[kubik\] devices connect via/g) ?? []).length;

/**
 * The first approval writes commands.ownerAllowFrom; OpenClaw hot-reloads that change and restarts the channel
 * 0.1 to 1 s later, depending on the version. Waiting for the restart keeps later steps from racing it (the
 * restart closes any socket that is open, which is what the device sees once, at its first pairing).
 */
async function waitForOwnerBootstrapRestart(startsBefore) {
  for (let attempt = 0; attempt < 100 && (await channelStarts()) <= startsBefore; attempt++) await sleep(100);
  assert.ok((await channelStarts()) > startsBefore, 'the kubik channel was not restarted after commands.ownerAllowFrom changed');
  await sleep(300);
}

async function pairAndReconnect() {
  const device = newDevice();
  const startsBefore = await channelStarts();
  const first = await pairThroughCli(device);
  if (first.sameSocket) say(`welcome on the same socket ${Math.round(performance.now() - first.approvedAt)} ms after approve`);
  else {
    const attempts = await reconnectUntilWelcome(device);
    ctx.warnings.push(`the first approval on a clean OpenClaw restarts the kubik channel (it writes commands.ownerAllowFrom); the device was welcomed after ${attempts} reconnect(s)`);
  }
  device.commitPin();
  device.close();
  await device.waitFor((e) => e.t === '__closed__', 5000);
  ctx.pin = device.pin;
  await waitForOwnerBootstrapRestart(startsBefore);
  // `pairing approve` bootstraps commands.ownerAllowFrom with the prefixed pairing entry; the device (bare-id sender) is never the owner.
  const { commands } = JSON.parse(await readFile(ctx.configFile, 'utf8'));
  assert.ok(!(commands?.ownerAllowFrom ?? []).includes(device.id), 'a device must never be a command owner');
  const again = newDevice(ctx.pin);
  await again.connect(ctx.lanUrl);
  assert.equal((await again.outcome(15_000)).t, 'welcome', 'the approved device must be welcomed without a new code');
  ctx.device = again;
}

async function secondDeviceSameSocket() {
  const second = new SimDevice({ id: `kubik-${randomBytes(3).toString('hex')}`, keyPem: newDeviceKeyPem() });
  const result = await pairThroughCli(second);
  assert.ok(result.sameSocket, 'once an owner exists, approval must reach the waiting device on the same socket');
  say(`second device: welcome on the same socket ${Math.round(performance.now() - result.approvedAt)} ms after approve`);
  second.close();
}

async function speechPcm() {
  const synthesize = createSynth({ log: () => {} });
  const speech = await synthesize('Привет, Кубик! Как у тебя дела?');
  const pcm = Buffer.alloc(Math.max(72_000, speech.length + 9600));
  speech.copy(pcm, 4800);
  return pcm;
}

async function assertVoiceTurn(device, label) {
  const before = { ...ctx.mock.stats };
  const reply = await device.pushToTalk(await speechPcm(), 1);
  assert.ok(reply.frames > 5 && reply.speechMs >= 500 && reply.rms > 0.005, `${label}: no audible IMA speech (${JSON.stringify(reply)})`);
  assert.ok(reply.states.includes('thinking') && reply.states.at(-1) === 'idle', `${label}: states ${reply.states}`);
  const { stats } = ctx.mock;
  assert.ok(stats.transcriptions > before.transcriptions && stats.chat > before.chat && stats.speech > before.speech, `${label}: STT, agent and TTS were not all used`);
  const request = await readFile(process.env.MOCK_DUMP_CHAT, 'utf8');
  const heard = STT_PHRASES.find((phrase) => request.includes(phrase));
  assert.ok(heard, `${label}: the agent's LLM request does not contain the transcript from STT`);
  say(`${label}: ${reply.frames} ADPCM frames, ${reply.speechMs} ms of speech, emotions [${reply.emotions}], the agent received "${heard}"`);
}

async function notification() {
  const command = documented(ctx.docs, 'openclaw message send --channel kubik', { 'kubik-xxxxxx': ctx.deviceId });
  const mark = ctx.device.events.length;
  await ctx.box.mustRun(command);
  const speak = await ctx.device.waitFor((e) => e.t === 'speak' && e.kind === 'notify', 30_000, mark);
  assert.equal(speak.kind, 'notify');
  await ctx.device.waitFor((e) => e.t === 'speak_end', 30_000, mark);
  ctx.device.close();
}

async function gatewayRoute() {
  const { ca, cert, key } = await createCa(ctx.dir);
  ctx.tls = { ca, cert, key };
  ctx.proxy = await startForwarder(ctx.tls, { port: ctx.box.gatewayPort, tls: false, forwardedFor: CLIENT_IP });
  const device = newDevice();
  await device.connect(`wss://127.0.0.1:${ctx.proxy.port}/kubik/v1`, { ca });
  assert.equal((await device.outcome(15_000)).t, 'welcome', 'the Gateway route with bind "ca:127.0.0.1" must accept the approved key');
  assert.equal(device.bind, 'ca:127.0.0.1');
  await assertVoiceTurn(device, 'gateway route /kubik/v1 (bind ca:127.0.0.1)');
  const log = await readFile(ctx.box.gatewayLog, 'utf8');
  assert.match(log, new RegExp(`connected from ${CLIENT_IP.replaceAll('.', '\\.')} via gateway`), 'the plugin must see the client address the Gateway resolved from X-Forwarded-For');
  device.close();
}

/** Connects through a forwarder to the Gateway route and reports how the server answered (`welcome` or the close code). */
async function routeOutcome(forwarder, { host = '127.0.0.1', fw } = {}) {
  const device = newDevice(null, fw);
  await device.connect(`wss://${host}:${forwarder.port}/kubik/v1`, { ca: ctx.tls.ca });
  const event = await device.outcome(15_000);
  device.close();
  return { bind: device.bind, result: event.t === '__closed__' ? event.code : event.t };
}

/** The signature names the host the device connected to: a relay, or a proxy that rewrites Host, must not pass. */
async function gatewayHostBinding() {
  const upstream = { port: ctx.box.gatewayPort, tls: false, forwardedFor: CLIENT_IP };
  const logNow = () => readFile(ctx.box.gatewayLog, 'utf8');
  // A hostless signature is refused, including old firmware.
  assert.deepEqual(await routeOutcome(ctx.proxy, { fw: '0.6.0' }), { bind: 'ca', result: 4001 });
  // A reverse proxy that talks to the Gateway by address but names the public host in X-Forwarded-Host (trusted: 127.0.0.1).
  const nginxLike = await startForwarder(ctx.tls, { ...upstream, host: `127.0.0.1:${ctx.box.gatewayPort}`, forwardedHost: 'localhost:443' });
  // Relays: the device signed the host it dialled (`ca:localhost`); the Gateway sees another one.
  const relay = await startForwarder(ctx.tls, { ...upstream, host: 'kubik.example.net' });
  const spoofing = await startForwarder(ctx.tls, { ...upstream, host: 'kubik.example.net', forwardedHost: 'localhost' });
  try {
    assert.deepEqual(await routeOutcome(nginxLike, { host: 'localhost' }), { bind: 'ca:localhost', result: 'welcome' });
    assert.deepEqual(await routeOutcome(relay, { host: 'localhost' }), { bind: 'ca:localhost', result: 4001 }, 'a handshake relayed as if it arrived on another host must be refused');
    assert.deepEqual(await routeOutcome(spoofing, { host: 'localhost' }), { bind: 'ca:localhost', result: 4001 }, 'X-Forwarded-Host must not override a public Host');
    assert.deepEqual(await routeOutcome(relay, { host: 'localhost', fw: '0.6.0' }), { bind: 'ca', result: 4001 }, 'hostless signatures never authorize a relay');
  } finally { await Promise.all([nginxLike, relay, spoofing].map((forwarder) => forwarder.close())); }
  assert.match(await logNow(), /transport binding mismatch.*this request arrived on host "kubik\.example\.net" \(Host header\)/);
}

async function relayIsRefused() {
  const relay = await startForwarder(ctx.tls, { port: Number(ctx.lanPort), tls: true });
  try {
    const url = `wss://127.0.0.1:${relay.port}/kubik/v1`;
    const trusting = newDevice(); // no pin stored: it would trust the relay's key, then sign it
    await trusting.connect(url);
    assert.notEqual(trusting.observedSpki, ctx.pin);
    const closed = await trusting.outcome(15_000);
    assert.equal(closed.t, '__closed__', `the relayed connection was not refused (${closed.t})`);
    assert.equal(closed.code, 4001);
    await assert.rejects(newDevice(ctx.pin).connect(url), PinMismatch);
  } finally { await relay.close(); }
}

async function serverKeyChanged() {
  await ctx.box.stopGateway();
  await rm(join(ctx.box.stateDir, 'kubik', 'lan-tls.json'));
  await ctx.box.startGateway();
  const device = newDevice(ctx.pin);
  await assert.rejects(device.connect(ctx.lanUrl), PinMismatch);
  const resaved = newDevice(); // README: saving the portal settings again clears the pin
  await resaved.connect(ctx.lanUrl);
  assert.notEqual(resaved.observedSpki, ctx.pin);
  assert.equal((await resaved.outcome(15_000)).t, 'welcome', 'an approved key keeps working after the pin is re-learned');
  resaved.commitPin();
  ctx.device = resaved;
  ctx.pin = resaved.pin;
  resaved.close();
  await resaved.waitFor((e) => e.t === '__closed__', 5000);
}

/** Case b of the manual: a VPS with a public IP. Runs the documented commands, restarts, connects with the pin by IP. */
async function publicListener() {
  await ctx.box.stopGateway();
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw config set channels.kubik.listen.enabled true'));
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw config set channels.kubik.listen.public true'));
  const status = await ctx.box.startGateway();
  const listener = readLanStatus(status, true);
  assert.ok(ctx.pin.startsWith(listener.spkiPrefix), 'the LAN key must survive the restart, or every pin breaks');
  const url = `wss://${listener.addresses[0].host}/kubik/v1`; // the copied kubik:// address on the device
  const known = newDevice(ctx.pin);
  await known.connect(url);
  assert.equal((await known.outcome(15_000)).t, 'welcome', 'an approved device must be welcomed by IP with its pin');
  known.close();
  const fresh = new SimDevice({ id: `kubik-${randomBytes(3).toString('hex')}`, keyPem: newDeviceKeyPem() });
  await fresh.connect(url);
  const pair = await fresh.outcome(15_000);
  assert.equal(pair.t, 'pair', `an unknown device on the public listener must get a code, got ${pair.t}`);
  assert.ok((await ctx.box.mustRun(documented(ctx.docs, 'openclaw pairing list kubik'))).includes(pair.code));
  fresh.close();
  ctx.warnings.push('the public listener was checked with a private client address (a test host has no public one): the peer filter itself is covered by openclaw-kubik unit tests');
}

async function revokedDevice() {
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw config set channels.kubik.devices.', { 'kubik-xxxxxx': ctx.deviceId }));
  let code = null;
  let attempts = 0;
  while (attempts < 5 && code !== 4001) {
    attempts++;
    const device = newDevice(ctx.pin);
    await device.connect(ctx.lanUrl);
    const event = await device.outcome(15_000);
    code = event.t === '__closed__' ? event.code : null;
    if (code !== 4001) { device.close(); await sleep(2000); }
  }
  assert.equal(code, 4001, 'a revoked device must be closed with 4001');
  say(`revoked device closed with 4001 after ${attempts} connection attempt(s), no gateway restart`);
}

const ownerAllowFrom = async () => JSON.parse(await readFile(ctx.configFile, 'utf8')).commands?.ownerAllowFrom;

/**
 * KIT.ru.md, "Удаление": the documented commands take the `kubik:` entries out of commands.ownerAllowFrom (which the
 * first `pairing approve` wrote), keep the customer's own, remove the plugin and its channel config, and a
 * restarted Gateway runs without it.
 */
async function uninstallFollowingDocs() {
  const own = 'telegram:123456789'; // the docs' example of a customer's own owner entry
  const bootstrapped = await ownerAllowFrom();
  assert.ok(bootstrapped?.length && bootstrapped.every((entry) => entry.startsWith('kubik:')), `the first approval must have left only kubik: entries, got ${JSON.stringify(bootstrapped)}`);
  assert.equal(bootstrapped.every((entry) => entry.split(':').length === 3), true, 'kubik:<id>:<fingerprint>');
  const seed = (entries) => ctx.box.mustRun(`openclaw config set commands.ownerAllowFrom '${JSON.stringify(entries)}' --strict-json`);
  // Only kubik entries: the documented `config unset` leaves no list at all.
  assert.match(await ctx.box.mustRun(documented(ctx.docs, 'openclaw config get commands.ownerAllowFrom')), /kubik:kubik-/);
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw config unset commands.ownerAllowFrom'));
  assert.equal(await ownerAllowFrom(), undefined);
  // The customer's own owner sits next to the kubik entries: the documented `config set` keeps just theirs.
  await seed([own, ...bootstrapped]);
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw config set commands.ownerAllowFrom'));
  assert.deepEqual(await ownerAllowFrom(), [own]);
  const uninstall = await ctx.box.mustRun(documented(ctx.docs, 'openclaw plugins uninstall kubik'));
  assert.match(uninstall, /Uninstalled plugin "kubik"/);
  const { channels, plugins } = JSON.parse(await readFile(ctx.configFile, 'utf8'));
  assert.equal(channels?.kubik, undefined, 'plugins uninstall must drop channels.kubik');
  assert.notEqual(plugins?.entries?.kubik?.enabled, true);
  assert.deepEqual(await ownerAllowFrom(), [own], 'uninstalling must not touch the customer\'s own owner entry');
  // The documented restart: no service here, so OpenClaw restarts the foreground Gateway (it exits) and a fresh one stands in.
  const restart = await ctx.box.run(documented(ctx.docs, 'openclaw gateway restart'));
  assert.match(restart.output, /service management skipped|not installed|not loaded|restart request sent/i, restart.output);
  await ctx.box.stopGateway();
  await ctx.box.startGateway(/Gateway reachable/);
  await stat(join(ctx.box.stateDir, 'kubik', 'lan-tls.json')); // what the docs say stays on disk
  await ctx.box.mustRun('openclaw config validate');
  const { plugins: listed } = JSON.parse(await ctx.box.mustRun('openclaw plugins list --json'));
  assert.ok(!listed.some((plugin) => plugin.id === 'kubik'), 'the Kubik plugin must be gone from the plugin list');
  await assert.rejects(newDevice(ctx.pin).connect(ctx.lanUrl), 'nothing may listen on the LAN port after uninstall');
}

/** KIT.ru.md: after an uninstall the approvals and the LAN key remain, and `plugins enable kubik` is needed before `channels add`. */
async function reinstallAfterUninstall() {
  await ctx.box.stopGateway();
  const archive = `openclaw-kubik-${ctx.pluginManifest.version}.tgz`;
  await ctx.box.mustRun(documented(ctx.docs, `openclaw plugins install ./${archive} `, { [`./${archive}`]: ctx.tgz }));
  const blocked = await ctx.box.run(documented(ctx.docs, 'openclaw channels add --channel kubik --enable'));
  assert.ok(!blocked.ok && /Unknown channel "kubik"/.test(blocked.output), `channels add must fail until the plugin is enabled again:\n${blocked.output}`);
  await ctx.box.mustRun(documented(ctx.docs, 'openclaw plugins enable kubik'));
  assert.match(await ctx.box.mustRun(documented(ctx.docs, 'openclaw channels add --channel kubik --enable')), /Added Kubik account "default"/);
  if (ctx.configuredPort) await ctx.box.mustRun(`openclaw config set channels.kubik.listen.port ${ctx.configuredPort}`);
  await ctx.box.startGateway();
  const device = newDevice(ctx.pin);
  await device.connect(ctx.lanUrl);
  assert.equal((await device.outcome(15_000)).t, 'welcome', 'the pin and the approval must survive an uninstall and a reinstall');
  device.close();
}

const steps = [
  ['preflight and isolation inputs', preflight], ['plugin archive', prepareKit], ['clean OpenClaw + mock providers', cleanOpenClaw],
  ['documented install, enable, status --probe', installFollowingDocs], ['UDP discovery', discoverLan],
  ['TOFU pin, pairing, welcome, pin-verified reconnect', pairAndReconnect], ['second device: welcome on the same socket', secondDeviceSameSocket], ['LAN voice turn', () => assertVoiceTurn(ctx.device, 'LAN turn')],
  ['notification (openclaw message send)', notification], ['Gateway route behind a CA-trusted TLS proxy', gatewayRoute],
  ['Gateway route binds the signature to the host (hostless rejection, X-Forwarded-Host, relay)', gatewayHostBinding],
  ['MITM relay is refused', relayIsRefused], ['server key changed', serverKeyChanged],
  ['case b: documented listen.public, connect by IP with the pin', publicListener], ['revoked device', revokedDevice],
  ['documented uninstall: ownerAllowFrom cleaned, plugin removed', uninstallFollowingDocs],
  ['reinstall after uninstall keeps the pin and approvals', reinstallAfterUninstall],
];

async function cleanup(failed) {
  if (failed && ctx.box) console.error(`--- gateway log tail ---\n${(await readFile(ctx.box.gatewayLog, 'utf8').catch(() => '')).split('\n').slice(-40).join('\n')}`);
  await ctx.box?.stopGateway().catch(() => {});
  await ctx.proxy?.close().catch(() => {});
  await ctx.mock?.close().catch(() => {});
  if (ctx.dir && !argv.includes('--keep')) await rm(ctx.dir, { recursive: true, force: true });
  else if (ctx.dir) say(`kept ${ctx.dir}`);
}

let failed = false;
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => cleanup(true).finally(() => process.exit(130)));
try {
  for (const [name, run] of steps) {
    const at = performance.now();
    await run();
    say(`PASS ${name} (${Math.round(performance.now() - at)} ms)`);
  }
  assert.deepEqual(await realStateFingerprint(), ctx.realBefore, 'the real ~/.openclaw changed during the test');
  for (const note of ctx.warnings) say(`note: ${note}`);
  say(`all ${steps.length} steps passed with OpenClaw ${ctx.openclawVersion} and openclaw-kubik ${ctx.pluginManifest.version}`);
} catch (error) {
  failed = true;
  console.error(`FAIL: ${error.stack ?? error}`);
} finally {
  await cleanup(failed);
}
process.exit(failed ? 1 : 0);
