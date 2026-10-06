// Copy this file and change the two requests in connect() and reply().
// Kubik speaks whatever text reply() returns. device.id is the conversation key.
import { defineAdapter, readJsonBounded, readSetupEnv } from 'openclaw-kubik/agent-sdk';

let endpoint = '';
let token = '';
const pending = new Set();

function assertEndpoint(value) {
  const url = new URL(value);
  const local = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname);
  if (url.username || url.password || url.search || url.hash ||
      !(url.protocol === 'https:' || (url.protocol === 'http:' && local))) {
    throw Error('Use HTTPS, or HTTP on localhost, without credentials, query or fragment');
  }
  return url.href.replace(/\/+$/, '');
}

async function request(path, body, signal) {
  const response = await fetch(endpoint + path, {
    method: body ? 'POST' : 'GET', redirect: 'error', signal,
    headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
    ...(body ? { body: JSON.stringify(body) } : {}),
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw Error(`Agent returned HTTP ${response.status}`);
  }
  return readJsonBounded(response, 64 * 1024, 'Agent');
}

export default defineAdapter({
  id: 'http-json',
  label: 'HTTP JSON example',
  setup: [
    { key: 'endpoint', label: 'Agent URL', type: 'string', required: true, default: 'http://127.0.0.1:8000' },
    { key: 'tokenEnv', label: 'Agent token variable', type: 'env', default: 'MY_AGENT_TOKEN' },
  ],
  async connect({ config, env, signal }) {
    endpoint = assertEndpoint(config.endpoint);
    token = readSetupEnv(config, 'tokenEnv', env);
    let health;
    try {
      health = await request('/health', null, AbortSignal.any([signal, AbortSignal.timeout(5000)]));
    } catch {
      throw Error('Agent health check failed');
    }
    if (health.ok !== true) throw Error('Agent health check failed');
  },
  async reply({ device, transcript, signal }) {
    if (pending.size >= 8) throw Error('Agent is busy');
    const marker = {};
    pending.add(marker);
    try {
      const body = await request('/turn', { session: device.id, text: transcript },
        AbortSignal.any([signal, AbortSignal.timeout(30000)]));
      if (signal.aborted) return;
      if (typeof body.text !== 'string' || !body.text.trim() || body.text.length > 8192) throw Error('Invalid reply');
      return body.text;
    } catch {
      if (signal.aborted) return;
      throw Error('Agent request failed');
    } finally {
      pending.delete(marker);
    }
  },
  close() { endpoint = ''; token = ''; },
});
