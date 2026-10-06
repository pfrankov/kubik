#!/usr/bin/env node
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { runCli } from '../openclaw-kubik/src/agent-sdk/cli.js';

export { collectSetupValues, parseArgs, runCli } from '../openclaw-kubik/src/agent-sdk/cli.js';

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  runCli().catch((error) => {
    process.stderr.write(`Ошибка: ${error?.message ?? error}\n`);
    process.exitCode = 1;
  });
}
