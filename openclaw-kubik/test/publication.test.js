import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { validatePublication, needsPublication } from '../scripts/publication-check.mjs';
const pkg = JSON.parse(readFileSync(new URL('../package.json', import.meta.url)));
const readme = readFileSync(new URL('../README.md', import.meta.url), 'utf8');
test('publication requires intended public repository and English npm documentation', () => {
  validatePublication(pkg, readme);
  for (const change of [{ private: true }, { name: 'other' }, { repository: { url: 'https://example.com/github.com/pfrankov/kubik' } }, { publishConfig: { access: 'restricted' } }])
    assert.throws(() => validatePublication({ ...pkg, ...change }, readme));
  assert.throws(() => validatePublication(pkg, `${readme}\nУстановка`), /English/);
});
test('publication never treats registry failures or an immutable-version mismatch as a new release', async () => {
  assert.equal(await needsPublication({ status: 404 }, 'same'), true);
  assert.equal(await needsPublication({ ok: true, json: async () => ({ dist: { integrity: 'same' } }) }, 'same'), false);
  await assert.rejects(needsPublication({ status: 503 }, 'same'), /503/);
  await assert.rejects(needsPublication({ ok: true, json: async () => ({ dist: { integrity: 'different' } }) }, 'same'), /bump/);
  await assert.rejects(needsPublication({ ok: true, json: async () => ({}) }, 'same'), /bump/);
});
