import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { chmod, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { runNpm } from '../scripts/npm-command.mjs';

test('nested npm keeps the lifecycle caller instead of the SDK dependency PATH binary', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'kubik-npm-caller-'));
  const previous = process.env.npm_execpath;
  t.after(async () => {
    if (previous === undefined) delete process.env.npm_execpath;else process.env.npm_execpath = previous;
    await rm(dir, { recursive: true, force: true });
  });
  const caller = join(dir, 'caller.mjs'), shadow = join(dir, 'npm'), marker = join(dir, 'shadow-used');
  await writeFile(caller, `process.stdout.write(JSON.stringify([{source:'caller',node:process.execPath,args:process.argv.slice(2)}]));`);
  await writeFile(shadow, `#!${process.execPath}\nimport fs from 'node:fs';fs.appendFileSync(${JSON.stringify(marker)},'used');process.stdout.write(JSON.stringify({package:'shadow'}));`);
  await chmod(shadow, 0o700);
  const options = { cwd: dir, env: { ...process.env, PATH: dir }, encoding: 'utf8' };
  const args = ['pack', '--ignore-scripts', '--json'];
  const wrong = JSON.parse(execFileSync('npm', args, options));assert.deepEqual(wrong, { package: 'shadow' });
  delete process.env.npm_execpath;
  assert.deepEqual(JSON.parse(runNpm(args, options)), { package: 'shadow' }, 'direct invocation uses its explicit PATH selection');
  await rm(marker);
  process.env.npm_execpath = caller;
  const [packed] = JSON.parse(runNpm(args, options));
  assert.deepEqual(packed, { source: 'caller', node: process.execPath, args });
  await assert.rejects(readFile(marker), { code: 'ENOENT' });

  await writeFile(caller, 'process.exit(13);');
  assert.throws(() => runNpm(args, options), (error) => error.status === 13);
  await assert.rejects(readFile(marker), { code: 'ENOENT' }, 'a failed caller is not retried through another npm');
});
