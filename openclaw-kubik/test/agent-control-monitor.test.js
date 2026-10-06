import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createAgentControl } from '../src/agent-control.js';
import { monitorAccount } from '../src/monitor.js';
import { account, config, fakePairing, fakeEngine, installRuntime } from './helpers.js';

test('monitor forwards its info logger to agent control diagnostics', async (t) => {
  const stateDir = mkdtempSync(join(tmpdir(), 'kubik-agent-control-'));
  t.after(() => rmSync(stateDir, { recursive: true, force: true }));
  const logs = [];
  const { core, sdk } = installRuntime({
    pairing: fakePairing().api,
    extraCore: {
      state: { resolveStateDir: () => stateDir },
      agent: { session: { getSessionEntry: () => undefined } },
    },
  });
  const cfg = config();
  const acc = account({ listen: { enabled: false } });
  const server = await monitorAccount({ account: acc, cfg, setStatus() {},
    log: { info: (message) => logs.push(message), warn: (message) => logs.push(message) } }, {
    core, sdk, engineFactory: () => fakeEngine(),
    cron: { subscribe: () => () => {}, refresh() {} },
    lan: { stateDir },
    createAgentControl: (options) => createAgentControl({ ...options, modelRuntime: {
      loadPreparedModelCatalog: async () => [{ provider: 'openai', id: 'gpt-5.2', name: 'GPT 5.2' }],
      resolveAllowedModelRef: () => ({ ref: { provider: 'openai', model: 'gpt-5.2' } }),
      resolveDefaultModelForAgent: () => ({ provider: 'openai', model: 'gpt-5.2' }),
    } }),
  });
  t.after(() => {
    globalThis[Symbol.for('openclaw.kubik.servers')]?.delete(acc.accountId);
    return server.stop();
  });

  await server.agentControl.selectModel({ id: 'kubik-b6c634' }, 'openai/gpt-5.2');
  assert.ok(logs.some((message) => message === 'kubik: model control kubik-b6c634 selection-start'));
});
