import assert from 'node:assert/strict';
import { SimDevice } from './device.mjs';

const TOOL = 'kubik_printer';
const TEXT = 'Printer fixture\nMilk\nХлеб';
const messageText = message => typeof message?.content === 'string' ? message.content :
  (message?.content ?? []).map(part => part.text ?? '').join('\n');
function toolMessage(messages, names) {
  const ids = new Set(messages.flatMap(message => (message.tool_calls ?? [])
    .filter(call => names.includes(call.function?.name)).map(call => call.id)));
  return [...messages].reverse().find(message => message.role === 'tool' && ids.has(message.tool_call_id));
}

function currentMessages(body, active) {
  const messages = body.messages ?? [];
  const start = messages.findLastIndex(message => message.role === 'user' && messageText(message).includes(`PRINT_FIXTURE_${active.status}:`));
  assert.ok(start >= 0, 'current printer request was not delivered to the model');
  return messages.slice(start);
}
const call = (id, name, args) => ({ role: 'assistant', content: null, tool_calls: [{ id, type: 'function',
  function: { name, arguments: JSON.stringify(args) } }] });

function discoveredCall(body, messages, active) {
  const names = body.tools?.map(tool => tool.function?.name) ?? [];
  const args = active.phase === 'status' ? { action: 'status' } :
    { action: 'print', text: TEXT, format: 'text', width_mm: 30, height_mm: 20, font_mm: 3 };
  const id = active.phase === 'status' ? `${active.callId}_status` : active.callId;
  if (names.includes(TOOL)) return call(id, TOOL, args);
  assert.ok(names.includes('tool_search') && names.includes('tool_describe') && names.includes('tool_call'),
    'neither the printer nor the SDK tool discovery surface is available to the model');
  const described = toolMessage(messages, ['tool_describe']);
  if (described) {
    const schema = messageText(described);
    assert.match(schema, /kubik_printer/); assert.match(schema, /action/); assert.match(schema, /text/);
    assert.ok(active.resultId, 'discovery did not provide an exact printer id');
    return call(id, 'tool_call', { id: active.resultId, args });
  }
  const searched = toolMessage(messages, ['tool_search']);
  if (searched) {
    const catalog = JSON.parse(messageText(searched));
    assert.ok(Array.isArray(catalog), 'tool search did not return its result catalog');
    const matches = catalog.filter(result => result.name === TOOL);
    assert.equal(matches.length, 1, 'printer was missing or ambiguous in the actual policy-filtered tool catalog');
    assert.equal(typeof matches[0].id, 'string'); active.resultId = matches[0].id;
    return call(`${id}_describe`, 'tool_describe', { id: active.resultId });
  }
  return call(`${id}_search`, 'tool_search', { query: TOOL, limit: 3 });
}

function fixturePrinter() {
  return { name: 'PeriPage+TEST', model: 'PeriPage A6+', firmware: 'V1.12_304dpi',
    hardware: 'v3.38.21_AY', dpi: 304, width_dots: 576, printable_width_mm: 576 * 25.4 / 304,
    battery_pct: 100, formats: ['text', 'qr'], max_height_mm: 150, media_height_mm: null, size_source: 'model_profile' };
}

function nextCall(body, messages, active) {
  const response = discoveredCall(body, messages, active);
  const stage = `${active.phase}:${response.tool_calls[0].function.name}`;
  assert.ok(!active.stages.has(stage), 'printer fixture repeated a completed tool stage');
  active.stages.add(stage);
  if (response.tool_calls[0].id === active.callId) assert.equal(active.calls++, 0, 'model fixture attempted the print more than once');
  return response;
}

/** Explicit hooks for the clean-install test only; ordinary STT/LLM behavior stays unchanged. */
export function createPrinterFixture() {
  let active = null;
  return {
    begin(status) {
      assert.equal(active, null);
      active = { status, callId: `printer_fixture_${status}`, calls: 0, modelRequests: 0,
        phase: 'status', statusCalls: 0, stages: new Set(), requests: [], speech: [], result: null };
      return active;
    },
    end() { active = null; },
    sttOverride: () => active ? `PRINT_FIXTURE_${active.status}: please print the shopping list.` : undefined,
    onSpeech(text) {
      if (!active) return;
      assert.ok(active.speech.length < 8, 'printer fixture generated too many speech blocks');
      active.speech.push(text);
    },
    chatReply(body) {
      if (!active) return null;
      assert.ok(++active.modelRequests <= 8, 'printer fixture exceeded its bounded model loop');
      const messages = currentMessages(body, active);
      const result = toolMessage(messages, [TOOL, 'tool_call']);
      if (result) {
        const text = messageText(result);
        if (active.phase === 'status') {
          assert.match(text, /"status"\s*:\s*"ready"/);
          assert.match(text, /"width_dots"\s*:\s*576/);assert.match(text, /"dpi"\s*:\s*304/);
          active.phase = 'print';
          return nextCall(body, messages, active);
        }
        assert.match(text, new RegExp(`"status"\\s*:\\s*"${active.status}"`), 'model did not receive the real device result');
        active.result = text;
        return { role: 'assistant', content: active.status === 'sent' ?
          'The text was sent to the printer. Paper output is not confirmed.' :
          'The printer was not found. Nothing was sent for printing.' };
      }
      return nextCall(body, messages, active);
    },
    printerReply(request) {
      assert.ok(active, 'unexpected printer request outside a fixture turn');
      if (request.action === 'status') {
        assert.equal(active.statusCalls++, 0);
        active.requests.push(request);
        return { status: 'ready', printer: fixturePrinter() };
      }
      assert.equal(request.action, 'print'); assert.equal(request.text, TEXT);
      assert.equal(request.format, 'text');assert.equal(request.width_mm, 30);
      assert.equal(request.height_mm, 20);assert.equal(request.font_mm, 3);
      assert.match(request.rid, /^[0-9a-f]{32}$/);
      active.requests.push(request);
      return active.status === 'sent' ? { status: 'sent', outcome: 'submitted', printer: fixturePrinter(),
        layout: { format: 'text', width_dots: 359, height_dots: 239,
          width_mm: 359 * 25.4 / 304, height_mm: 239 * 25.4 / 304, font_mm: 3 } } :
        { status: 'error', code: 'printer_not_found', outcome: 'not_sent' };
    },
  };
}

/** Normal Kubik voice dispatch must preserve its trusted channel/id through the actual OpenClaw model loop. */
export async function assertPrinterModelTurns(ctx, pcm, say) {
  const previous = ctx.device;
  previous.close();
  await previous.waitFor(event => event.t === '__closed__', 5000);
  const device = new SimDevice({ id: ctx.deviceId, keyPem: ctx.keyPem, pin: ctx.pin,
    printerReply: request => ctx.printerFixture.printerReply(request) });
  ctx.device = device;
  try {
    await device.connect(ctx.lanUrl);
    assert.equal((await device.outcome(15000)).t, 'welcome');
    for (const [index, status] of ['sent', 'error'].entries()) {
      const run = ctx.printerFixture.begin(status);
      try {
        const reply = await device.pushToTalk(pcm, 20 + index);
        assert.equal(run.calls, 1); assert.equal(run.statusCalls, 1);assert.equal(run.requests.length, 2);
        assert.ok(run.result, 'tool result did not return to the model');
        assert.ok(reply.frames > 5 && reply.rms > 0.005, 'printer result did not reach audible device speech');
        const text = run.speech.join(' ');
        assert.match(text, status === 'sent' ? /sent to the printer.*not confirmed/i : /not found.*Nothing was sent/i);
        assert.doesNotMatch(text, /successfully printed|paper printed/i);
        say(`printer ${status}: actual model tool call, capability query, one print request and honest final reply`);
      } finally { ctx.printerFixture.end(); }
    }
  } catch (error) {
    device.close();
    throw error;
  }
}
