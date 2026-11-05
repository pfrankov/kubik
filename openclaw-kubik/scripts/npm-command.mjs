import { execFileSync } from 'node:child_process';

/** Lifecycle PATH includes dependency bins. Keep the npm that started the command. */
export function runNpm(args, options) {
  const caller = process.env.npm_execpath;
  return caller ? execFileSync(process.execPath, [caller, ...args], options) : execFileSync('npm', args, options);
}
