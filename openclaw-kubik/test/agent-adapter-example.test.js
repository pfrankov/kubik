import assert from 'node:assert/strict';
import test from 'node:test';
import http from 'node:http';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync, symlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { commandPrefix, shellQuote } from '../src/agent-sdk/cli.js';
import { normalizeSetupValues, validateAdapter, readJsonBounded } from '../src/agent-sdk/index.js';
import adapter from '../src/agent-sdk/examples/http-json.js';

test('printed POSIX commands work from a local bin symlink with spaces and no bin on PATH', {skip: process.platform === 'win32'}, t => {
  const dir = mkdtempSync(join(tmpdir(), "kubik cli's "));
  t.after(() => rmSync(dir, {recursive: true, force: true}));
  const bin = join(dir, 'node_modules/.bin'); mkdirSync(bin, {recursive: true});
  const script = join(bin, 'kubik-agent-host');
  symlinkSync(fileURLToPath(new URL('../src/agent-sdk/cli.js', import.meta.url)), script);
  const prefix = commandPrefix(script);
  const output = execFileSync('/bin/sh', ['-c', prefix + ' --help'], {encoding: 'utf8', cwd: dir, env: {...process.env, PATH: '/usr/bin:/bin'}});
  assert.ok(output.includes(prefix + ' [--state-dir PATH] serve'));
  const fromHelp = output.split('\n').find(line => line.endsWith(' setup')).trim().replace(' [--state-dir PATH] setup', ' --help');
  assert.match(execFileSync('/bin/sh', ['-c', fromHelp], {encoding: 'utf8', cwd: dir}), /Kubik agent host/);
});

test('Windows command hints use PowerShell invocation and literal argument quoting', () => {
  assert.equal(shellQuote("C:\\owner's folder\\node.exe", 'win32'), "'C:\\owner''s folder\\node.exe'");
  assert.ok(commandPrefix("cli's.js", "C:\\Node JS\\node.exe", 'win32').startsWith("& 'C:\\Node JS\\node.exe' "));
  assert.match(commandPrefix("cli's.js", 'node', 'win32'), /cli''s\.js'$/);
});

test('minimal adapters require only connect and dispatch, optional lifecycle hooks remain checked', () => {
  const minimal = {id: 'example', label: 'Example', setup: [], connect() {}, dispatch() {}};
  assert.equal(validateAdapter(minimal), minimal);
  for (const hook of ['start', 'close']) assert.throws(() => validateAdapter({...minimal, [hook]: true}), /must be a function/);
  for (const hook of ['connect', 'dispatch']) assert.throws(() => validateAdapter({...minimal, [hook]: undefined}), /must implement/);
});

test('bounded JSON rejects invalid limits before reading a body', async () => {
  for (const limit of [NaN, 0, -1, Infinity, 1.5, 2 * 1024 * 1024 + 1]) {
    await assert.rejects(readJsonBounded(new Response('{}'), limit, 'Example'), /limit/);
  }
});

test('shipped HTTP adapter: probe, device isolation, stale reply, bounded errors and close cancellation', async t => {
  let mode = 'ok'; const turns = [], errors = [], replies = [], requests = [];
  const waiting = [];
  const server = http.createServer((req, res) => {
    assert.equal(req.headers.authorization, 'Bearer example-test-token');
    requests.push(req.url);
    if (req.url === '/health') { res.end(mode === 'junk' ? 'not-json secret-body' : '{"ok":true}'); return; }
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      turns.push(JSON.parse(body));
      if (mode === 'hold') { waiting.push(res); return; }
      if (mode === 'error') { res.writeHead(500); res.end('private provider data'); return; }
      if (mode === 'huge') { res.end(JSON.stringify({text: 'x'.repeat(70000)})); return; }
      res.end(JSON.stringify({text: 'Reply to ' + JSON.parse(body).text}));
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await adapter.close(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); });
  const config = normalizeSetupValues(adapter.setup, {endpoint: `http://127.0.0.1:${server.address().port}`});
  await adapter.connect({config, env: {MY_AGENT_TOKEN: 'example-test-token'}});
  const turn = (id, text, current = () => true) => adapter.dispatch({device: {id}, transcript: text,
    speak: async text => replies.push(text), isCurrent: current, onAgentError: e => errors.push(e.message)});
  await turn('one', 'Hello'); await turn('two', 'World');
  assert.deepEqual(turns, [{session: 'one', text: 'Hello'}, {session: 'two', text: 'World'}]);
  assert.deepEqual(replies, ['Reply to Hello', 'Reply to World']);
  const count = requests.length; await turn('one', 'Stale', () => false); assert.equal(requests.length, count);
  mode = 'error'; await turn('one', 'Failed'); mode = 'huge'; await turn('one', 'Large');
  assert.deepEqual(errors, ['Agent request failed', 'Agent request failed']);
  mode = 'junk';
  await assert.rejects(adapter.connect({ config, env: { MY_AGENT_TOKEN: 'example-test-token' } }), (error) => {
    assert.equal(error.message, 'Agent health check failed');
    assert.equal(error.message.includes('not-json'), false);
    return true;
  });
  mode = 'ok';
  await adapter.connect({ config, env: { MY_AGENT_TOKEN: 'example-test-token' } });
  mode = 'hold'; let current = true;
  const job = turn('one', 'Cancelled', () => current);
  for (let i = 0; i < 100 && !waiting.length; i++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(waiting.length, 1);
  current = false; await adapter.close(); await job;
  assert.equal(replies.length, 2); assert.equal(errors.length, 2);
});

test('generic model control validates UTF-8 catalogs and requires persisted readback', async () => {
  const {createAdapterControl} = await import('../src/agent-sdk/controls.js');
  const voice = {apiKey: 'private', transcribeModel: 'stt', ttsModel: 'tts'};
  const empty = createAdapterControl({}, voice);
  assert.deepEqual((await empty.options({id: 'a'})).models, []);
  const modes = await empty.options({id: 'a'}, 'mode');
  assert.equal(modes.model, 'classic');
  assert.deepEqual(modes.models.map(row => row.available), [true, false, false]);
  await assert.rejects(empty.selectModel({}, 'live', 'mode'), {code: 'unsupported'});
  assert.deepEqual((await empty.options({}, 'stt')).models, []);
  await assert.rejects(empty.selectModel({}, 'x'), {code: 'unsupported'});
  let data = {model: 'x', models: [{id: 'x', label: 'X'}, {id: 'y', label: 'Y'}]};
  const control = createAdapterControl({modelOptions: async () => data, selectModel: async () => {}}, voice);
  await assert.rejects(control.selectModel({}, 'y'), {code: 'unavailable'});
  data = {model: 'x', models: [{id: 'x', label: 'Я'.repeat(41)}]};
  await assert.rejects(control.options({}), {code: 'unavailable'});
  data = {model: 'x', models: [{id: 'x', label: 'X'}, {id: 'x', label: 'Duplicate'}]};
  await assert.rejects(control.options({}), {code: 'unavailable'});
});

test('host next commands retain the explicit credential file without leaking unrelated Node flags', () => {
  const prefix = commandPrefix('/tmp/cli.js', 'node', 'linux', ['--inspect', '--env-file=/tmp/private credentials.env']);
  assert.equal(prefix, "'node' '--env-file=/tmp/private credentials.env' '/tmp/cli.js'");
});
