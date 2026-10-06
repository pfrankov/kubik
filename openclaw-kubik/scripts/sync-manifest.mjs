// Regenerates openclaw.plugin.json and the package.json setup-field projection from runtime code
// (src/config.js and src/channel-setup.js are the single source of truth). `--check` fails instead of writing.
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { CHANNEL_ID, channelSchema, uiHints } from '../src/config.js';
import { setupPlugin } from '../src/channel-setup.js';

const root = new URL('../', import.meta.url);
const check = process.argv.includes('--check');
const pkgPath = fileURLToPath(new URL('package.json', root));
const pkgText = await readFile(pkgPath, 'utf8');
const pkg = JSON.parse(pkgText);

const manifest = {
  id: CHANNEL_ID, name: 'Kubik', version: pkg.version,
  description: 'Kubik voice desk device channel (push-to-talk ESP32-C6, OpenAI-compatible voice)',
  channels: [CHANNEL_ID], categories: ['channels'],
  configSchema: { type: 'object', additionalProperties: false, properties: {} },
  channelConfigs: { [CHANNEL_ID]: { schema: channelSchema, uiHints } },
};
pkg.openclaw.channel.setup = { fields: setupPlugin.setupContract.metadata.fields.map((field) => ({ ...field })) };
const outputs = [
  [fileURLToPath(new URL('openclaw.plugin.json', root)), `${JSON.stringify(manifest, null, 2)}\n`],
  [pkgPath, `${JSON.stringify(pkg, null, 2)}\n`],
];
let stale = false;
for (const [path, text] of outputs) {
  const current = await readFile(path, 'utf8').catch(() => '');
  if (current === text) continue;
  if (check) { console.error(`${path} is out of sync; run npm run sync:manifest`); stale = true; }
  else { await writeFile(path, text); console.log(`Wrote ${path}`); }
}
if (stale) process.exit(1);
if (check) console.log('openclaw.plugin.json and package.json setup fields are in sync');
