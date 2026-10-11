// Publish exactly the reviewed tarball; an existing version must have identical integrity.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { validatePublication, needsPublication } from './publication-check.mjs';

const pkg = JSON.parse(readFileSync('package.json', 'utf8'));
validatePublication(pkg, readFileSync('README.md', 'utf8'));
const directory = mkdtempSync(join(tmpdir(), 'kubik-publish-'));
try {
  const [packed] = JSON.parse(execFileSync('npm', ['pack', '--ignore-scripts', '--json', '--pack-destination', directory], { encoding:'utf8' }));
  const response = await fetch(`https://registry.npmjs.org/${pkg.name}/${pkg.version}`, { signal:AbortSignal.timeout(15000) });
  if (await needsPublication(response, packed.integrity)) {
    execFileSync('npm', ['publish', join(directory, packed.filename), '--access', 'public'], { stdio:'inherit' });
  } else console.log(`${pkg.name}@${pkg.version} is already published with identical contents`);
} finally { rmSync(directory, { recursive:true, force:true }); }
