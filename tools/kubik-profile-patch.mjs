#!/usr/bin/env node
// Prints the JSON patch for the `kubik` OpenClaw profile (piped into `openclaw --profile kubik config patch --stdin`).
// Secrets come from tools/dev-secrets.json; nothing is printed to logs by dev-up.sh.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const s = JSON.parse(readFileSync(join(HERE, 'dev-secrets.json'), 'utf8'));
const mockPort = Number(process.env.MOCK_OPENAI_PORT ?? 18800);
const gatewayPort = Number(process.env.KUBIK_GATEWAY_PORT ?? 18789);
const devicePort = Number(process.env.KUBIK_DEVICE_PORT ?? 18790);
const base = `http://127.0.0.1:${mockPort}/v1`;

process.stdout.write(JSON.stringify({
  gateway: { mode: 'local', port: gatewayPort, bind: 'loopback', auth: { mode: 'token', token: s.gatewayToken } },
  models: { providers: { mock: {
    baseUrl: base, apiKey: s.mockApiKey, api: 'openai-completions',
    models: [{ id: 'mock-llm', name: 'Mock LLM (local)', reasoning: false, input: ['text'], contextWindow: 128000, maxTokens: 4096 }],
  } } },
  agents: { defaults: { model: { primary: 'mock/mock-llm' } } },
  plugins: { allow: null, entries: { kubik: { enabled: true } } }, // enabled by id; no allowlist (it would disable bundled plugins)
  channels: { kubik: {
    enabled: true,
    listen: { port: devicePort }, // LAN TLS listener; the Gateway also serves /kubik/v1
    voice: { provider: 'openai-http', baseUrl: base, apiKey: s.mockApiKey },
    allowInsecureBaseUrl: true,
    defaultTo: `kubik:${s.deviceId}`,
  } },
}));
