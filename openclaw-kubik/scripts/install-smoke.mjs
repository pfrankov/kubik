import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { randomInt } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(fileURLToPath(new URL('../', import.meta.url)));
const cli = join(root, 'node_modules', '.bin', 'openclaw');
const dir = await mkdtemp(join(tmpdir(), 'kubik-install-smoke-'));
const env = {
  ...process.env,
  OPENCLAW_STATE_DIR: join(dir, 'state'),
  OPENCLAW_CONFIG_PATH: join(dir, 'openclaw.json'),
  OPENCLAW_GATEWAY_PORT: String(randomInt(41000, 61000)),
};
const runCli = (args) => {
  const result = spawnSync(cli, args, { cwd: dir, env, encoding: 'utf8', timeout: 120_000 });
  assert.equal(result.status, 0, result.stderr);
  assert.doesNotMatch(result.stdout + result.stderr, /kubik failed during load|ERR_REQUIRE_ASYNC_MODULE/);
  return result.stdout;
};

try {
  const [pack] = JSON.parse(execFileSync('npm', ['pack', '--ignore-scripts', '--json', '--pack-destination', dir], {
    cwd: root, encoding: 'utf8',
  }));
  assert.equal(pack.name, 'openclaw-kubik');
  assert.ok(pack.files.some((file) => file.path === 'openclaw.plugin.json'));
  assert.ok(pack.files.some((file) => file.path === 'src/index.js'));
  assert.ok(pack.files.some((file) => file.path === 'LICENSE'));

  const artifact = join(dir, pack.filename);
  const installed = runCli(['plugins', 'install', `npm-pack:${artifact}`, '--force', '--accept-capabilities']);
  assert.match(installed, /Installed plugin: kubik/);

  const inspection = JSON.parse(runCli(['plugins', 'inspect', 'kubik', '--json']));
  assert.equal(inspection.plugin.id, 'kubik');
  assert.equal(inspection.plugin.packageVersion, pack.version);
  assert.ok(inspection.plugin.channelIds.includes('kubik'));
  assert.equal(inspection.plugin.dependencyStatus.dependencies.find((dependency) => dependency.name === 'ws')?.installed, true);

  const added = runCli(['channels', 'add', '--channel', 'kubik', '--enable']);
  assert.match(added, /Added Kubik account "default"/);
  const config = JSON.parse(await readFile(env.OPENCLAW_CONFIG_PATH, 'utf8'));
  assert.equal(config.plugins.entries.kubik.enabled, true);
  assert.equal(config.channels.kubik.enabled, true);
  const loaded = JSON.parse(runCli(['plugins', 'list', '--json'])).plugins.find(plugin => plugin.id === 'kubik');
  assert.equal(loaded?.status, 'loaded', loaded?.error);
  console.log(`Clean install passed: OpenClaw ${inspection.plugin.builtWithOpenClawVersion}, openclaw-kubik ${pack.version}, managed ws dependency, channel enabled through channels add --enable`);
} finally {
  await rm(dir, { recursive: true, force: true });
}
