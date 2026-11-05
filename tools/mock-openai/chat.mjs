import { randomUUID } from 'node:crypto';
import { json, sleep } from './http-utils.mjs';

function completion(body, message) {
  const reply = message.content ?? '';
  const usage = { prompt_tokens: Math.ceil(JSON.stringify(body.messages ?? []).length / 4), completion_tokens: Math.ceil(reply.length / 4) };
  usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
  return { id: `chatcmpl-${randomUUID()}`, created: Math.floor(Date.now() / 1000), model: body.model,
    reply, message, usage, finish: message.tool_calls?.length ? 'tool_calls' : 'stop' };
}

async function stream(res, value, delayMs, includeUsage) {
  const { id, created, model, message, reply, finish, usage } = value;
  res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
  const chunk = (delta, reason = null) => res.write(`data: ${JSON.stringify({ id, object: 'chat.completion.chunk', created, model,
    choices: [{ index: 0, delta, finish_reason: reason }] })}\n\n`);
  chunk({ role: 'assistant', content: '' });
  if (message.tool_calls?.length) chunk({ tool_calls: message.tool_calls.map((call, index) => ({ ...call, index })) });
  const pieces = reply.match(/\S+\s*|\s+/g) ?? [reply];
  for (let i = 0; i < pieces.length; i += 2) {
    if (res.destroyed) return;
    chunk({ content: pieces.slice(i, i + 2).join('') });
    await sleep(delayMs);
  }
  chunk({}, finish);
  if (includeUsage) res.write(`data: ${JSON.stringify({ id, object: 'chat.completion.chunk', created, model, choices: [], usage })}\n\n`);
  res.end('data: [DONE]\n\n');
}

/** Emit the same OpenAI-compatible mock response for normal text and explicit fixture tool calls. */
export async function sendChat(res, body, message, delayMs) {
  const value = completion(body, message);
  if (body.stream) return stream(res, value, delayMs, body.stream_options?.include_usage);
  const { id, created, model, usage } = value;
  return json(res, 200, { id, object: 'chat.completion', created, model,
    choices: [{ index: 0, message, finish_reason: value.finish }], usage });
}
