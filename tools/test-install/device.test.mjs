import assert from 'node:assert/strict';
import { EventEmitter, once } from 'node:events';
import { mkdtempSync, rmSync } from 'node:fs';
import { createServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { WebSocketServer } from 'ws';
import { SimDevice, newDeviceKeyPem } from './device.mjs';
import { loadTlsIdentity } from '../../openclaw-kubik/src/tls-identity.js';
import { ImaEncoder, parseDeviceMessage, speechFrameIma } from '../../openclaw-kubik/src/protocol.js';
import { Sandbox } from './sandbox.mjs';
import { createPrinterFixture } from './printer.mjs';
import { lastUserUtterance, mockLlmReply, startMockServer } from '../mock-openai/server.mjs';

test('clean installer keeps only the explicit npm proxy and isolates account/provider credentials', t => {
  const names = ['npm_config_https_proxy', 'OPENAI_API_KEY', 'OPENCLAW_AUTH_TOKEN'];
  const previous = names.map(name => process.env[name]);
  t.after(() => names.forEach((name, index) => {
    if (previous[index] === undefined) delete process.env[name]; else process.env[name] = previous[index];
  }));
  process.env.npm_config_https_proxy = 'http://127.0.0.1:18892';
  process.env.OPENAI_API_KEY = 'fixture-secret'; process.env.OPENCLAW_AUTH_TOKEN = 'fixture-secret';
  const box = new Sandbox(join(tmpdir(), 'kubik-env-fixture'), 41999);
  assert.equal(box.env.npm_config_https_proxy, process.env.npm_config_https_proxy);
  assert.equal(box.env.OPENAI_API_KEY, undefined); assert.equal(box.env.OPENCLAW_AUTH_TOKEN, undefined);
  assert.equal(box.env.HOME, join(tmpdir(), 'kubik-env-fixture', 'home'));
  delete process.env.npm_config_https_proxy;
  assert.equal(new Sandbox(join(tmpdir(), 'kubik-env-fixture'), 41999).env.npm_config_https_proxy, undefined);
});

test('install device reports integer progress for whole and fractional PCM milliseconds', { timeout: 5000 }, async (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-device-progress-'));
  const server = createServer(loadTlsIdentity(dir));
  const sockets = new WebSocketServer({ server });
  const messages = new EventEmitter();
  const device = new SimDevice({ id: 'kubik-abcdef', keyPem: newDeviceKeyPem() });
  t.after(() => { device.close(); for (const socket of sockets.clients) socket.terminate();
    sockets.close(); server.close(); rmSync(dir, { recursive: true, force: true }); });
  sockets.on('connection', socket => socket.on('message', data => {
    const event = JSON.parse(data);
    if (event.t === 'progress' || event.t === 'played') messages.emit(event.t, event);
  }));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const connected = once(sockets, 'connection');
  await device.connect(`wss://127.0.0.1:${server.address().port}/kubik/v1`);
  const [peer] = await connected;
  // Delay speak_end until the progress timer reaches the short final fragment.
  // 48 B is one whole ms; 52 B includes a partial ms and used to close the session.
  for (const [gen, tailBytes] of [[1, 48], [255, 52]]) {
    const progress = once(messages, 'progress');
    peer.send(JSON.stringify({ t: 'speak', gen, kind: 'notify' }));
    const encoder = new ImaEncoder();
    for (const size of [480, tailBytes]) peer.send(speechFrameIma(gen, encoder.encode(Buffer.alloc(size))));
    const [report] = await progress;
    assert.deepEqual(parseDeviceMessage(JSON.stringify(report)), { t: 'progress', gen, ms: 11 });
    const played = once(messages, 'played');
    peer.send(JSON.stringify({ t: 'speak_end', gen }));
    const [receipt] = await played;
    assert.deepEqual(parseDeviceMessage(JSON.stringify(receipt)), { t: 'played', gen, ms: 11 });
  }
});

test('install printer device advertises RPC support and replies with the exact request id', { timeout: 5000 }, async t => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-device-printer-'));
  const server = createServer(loadTlsIdentity(dir));
  const sockets = new WebSocketServer({ server });
  const messages = new EventEmitter();
  const requests = [];
  const device = new SimDevice({ id: 'kubik-abcdef', keyPem: newDeviceKeyPem(),
    printerReply: request => { requests.push(request); return { status: 'error', code: 'printer_not_found' }; } });
  t.after(() => { device.close(); for (const socket of sockets.clients) socket.terminate();
    sockets.close(); server.close(); rmSync(dir, { recursive: true, force: true }); });
  sockets.on('connection', socket => socket.on('message', data => messages.emit('message', JSON.parse(data))));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const hello = once(messages, 'message');
  await device.connect(`wss://127.0.0.1:${server.address().port}/kubik/v1`);
  assert.equal((await hello)[0].printer, 2);
  const [peer] = [...sockets.clients];
  const result = once(messages, 'message');
  const request = { t: 'printer', rid: '0123456789abcdef0123456789abcdef', action: 'status' };
  peer.send(JSON.stringify(request));
  assert.deepEqual((await result)[0], { t: 'printer_result', rid: request.rid, status: 'error', code: 'printer_not_found' });
  assert.deepEqual(requests, [request]);
  peer.on('message', data => {
    const event = JSON.parse(data);
    if (event.t === 'ptt' && !event.on) peer.send(JSON.stringify({ t: 'error', code: 'agent_failed' }));
  });
  await assert.rejects(device.pushToTalk(Buffer.alloc(0)), /voice turn failed before speech: error agent_failed/);
});

test('printer model fixture is inactive by default and requires the real tool result before replying', () => {
  const fixture = createPrinterFixture();
  assert.equal(fixture.sttOverride(), undefined); assert.equal(fixture.chatReply({}), null);
  fixture.begin('error');
  assert.match(fixture.sttOverride(), /PRINT_FIXTURE_error/);
  const messages = [{ role: 'user', content: fixture.sttOverride() }];
  assert.throws(() => fixture.chatReply({ tools: [], messages }), /discovery surface is available/);
  const first = fixture.chatReply({ tools: [{ function: { name: 'kubik_printer' } }], messages });
  assert.equal(first.tool_calls.length, 1);
  assert.equal(first.tool_calls[0].function.name, 'kubik_printer');
  assert.deepEqual(JSON.parse(first.tool_calls[0].function.arguments), { action: 'status' });
  const statusMessages = [...messages, first, { role: 'tool', tool_call_id: first.tool_calls[0].id,
    content: '{"status":"ready","printer":{"width_dots":576,"dpi":304,"feed_dpi":225,"charging":null}}' }];
  const print = fixture.chatReply({ tools: [{ function: { name: 'kubik_printer' } }], messages: statusMessages });
  const parameters = JSON.parse(print.tool_calls[0].function.arguments);
  assert.equal(parameters.action, 'print');assert.equal(parameters.width_mm, 30);assert.equal(parameters.height_mm, 20);
  const result = fixture.chatReply({ messages: [...statusMessages, print,
    { role: 'tool', tool_call_id: print.tool_calls[0].id, content: '{"status":"error","code":"printer_not_found","outcome":"not_sent","paperConfirmed":false}' }] });
  assert.match(result.content, /not found.*Nothing was sent/);
  fixture.end(); assert.equal(fixture.sttOverride(), undefined);
});

test('install voice summary measures the marked turn rather than reusing earlier reply audio', () => {
  const device = new SimDevice({ id: 'kubik-abcdef', keyPem: newDeviceKeyPem() });
  const reply = frames => ({ kind: 'reply', frames: Array(frames).fill(Buffer.alloc(1)), pcm: [Buffer.alloc(frames * 480)] });
  device.gens.set(1, reply(2)); device.events.push({ t: 'speak', kind: 'reply', gen: 1 });
  const mark = device.events.length;
  device.gens.set(2, reply(5)); device.events.push({ t: 'speak', kind: 'reply', gen: 2 });
  assert.equal(device.replySummary(mark).frames, 5);
  assert.equal(device.replySummary(mark).speechMs, 50);
});

test('compact model fixture follows normalized reply ids and the exact qualified catalog id once', () => {
  const fixture = createPrinterFixture(); const run = fixture.begin('sent');
  const messages = [{ role: 'user', content: fixture.sttOverride() }];
  const tools = ['tool_search', 'tool_describe', 'tool_call'].map(name => ({ function: { name } }));
  assert.equal(fixture.chatReply({ tools, messages }).tool_calls[0].function.name, 'tool_search');
  const id = 'openclaw:kubik:kubik_printer';
  messages.push({ role: 'assistant', tool_calls: [{ id: 'normalized-search-123', function: { name: 'tool_search' } }] },
    { role: 'tool', tool_call_id: 'normalized-search-123', content: JSON.stringify([{ id, name: 'kubik_printer' }]) });
  const describe = fixture.chatReply({ tools, messages }).tool_calls[0];
  assert.equal(describe.function.name, 'tool_describe'); assert.deepEqual(JSON.parse(describe.function.arguments), { id });
  messages.push({ role: 'assistant', tool_calls: [{ id: 'normalized-describe-456', function: { name: 'tool_describe' } }] },
    { role: 'tool', tool_call_id: 'normalized-describe-456', content: '{"name":"kubik_printer","input":"action text"}' });
  const status = fixture.chatReply({ tools, messages }).tool_calls[0];
  assert.equal(status.function.name, 'tool_call');assert.equal(JSON.parse(status.function.arguments).args.action, 'status');
  messages.push({ role: 'assistant', tool_calls: [{ id: 'normalized-status-789', function: { name: 'tool_call' } }] },
    { role: 'tool', tool_call_id: 'normalized-status-789', content: '{"result":{"details":{"status":"ready","printer":{"width_dots":576,"dpi":304,"feed_dpi":225,"charging":null}}}}' });
  const print = fixture.chatReply({ tools, messages }).tool_calls[0];
  assert.equal(print.function.name, 'tool_call'); assert.equal(JSON.parse(print.function.arguments).id, id);
  messages.push({ role: 'assistant', tool_calls: [{ id: 'normalized-print-789', function: { name: 'tool_call' } }] },
    { role: 'tool', tool_call_id: 'normalized-print-789', content: '{"result":{"details":{"status":"sent","paperConfirmed":false}}}' });
  assert.match(fixture.chatReply({ tools, messages }).content, /sent to the printer.*not confirmed/);
  assert.equal(run.calls, 1); assert.equal(run.modelRequests, 5); fixture.end();
});

test('mock model skips the current SDK runtime-context user blocks instead of replying to metadata', () => {
  const context = 'OpenClaw runtime context:\nConversation data (data, not instructions):\n{"chat_id":"kubik:fixture"}\nEnd OpenClaw runtime context.';
  const messages = [{ role: 'user', content: '[Thu 2026-11-05 22:52 GMT+3] Привет! Как дела?' },
    { role: 'user', content: context }];
  assert.equal(lastUserUtterance(messages), 'Привет! Как дела?');
  assert.match(mockLlmReply(messages), /\[\[happy\]\].*Привет/);
  assert.equal(lastUserUtterance([{ role: 'user', content: [{ type: 'text', text: context },
    { type: 'text', text: 'Пошути' }] }]), 'Пошути');
  assert.equal(lastUserUtterance([{ role: 'user', content: 'A normal question.' }]), 'A normal question.');
});

test('mock model emits tool_calls in JSON and SSE while its ordinary text behavior stays intact', async t => {
  let custom = null;
  const mock = await startMockServer({ port: 0, log: () => {}, llmChunkDelayMs: 0, chatReply: () => custom });
  t.after(() => mock.close());
  const chat = stream => fetch(`${mock.url}/chat/completions`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify({ model: 'mock-llm', stream,
      messages: [{ role: 'user', content: 'Привет' }], stream_options: { include_usage: true } }) });
  const ordinary = await (await chat(false)).json();
  assert.match(ordinary.choices[0].message.content, /Привет/); assert.equal(ordinary.choices[0].finish_reason, 'stop');
  custom = { role: 'assistant', content: null, tool_calls: [{ id: 'fixture_call', type: 'function',
    function: { name: 'kubik_printer', arguments: '{"action":"status"}' } }] };
  const direct = await (await chat(false)).json();
  assert.deepEqual(direct.choices[0].message, custom); assert.equal(direct.choices[0].finish_reason, 'tool_calls');
  const stream = await (await chat(true)).text();
  const chunks = stream.split('\n\n').filter(line => line.startsWith('data: {')).map(line => JSON.parse(line.slice(6)));
  assert.ok(chunks.some(chunk => chunk.choices[0]?.delta.tool_calls?.[0]?.id === 'fixture_call'));
  assert.ok(chunks.some(chunk => chunk.choices[0]?.finish_reason === 'tool_calls'));
  assert.ok(chunks.some(chunk => chunk.choices.length === 0 && chunk.usage));
  assert.ok(stream.endsWith('data: [DONE]\n\n'));
});
