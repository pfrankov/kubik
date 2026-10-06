// Reads the commands a new customer is told to type, straight from the customer manual, so the test fails when the
// documented flow drifts from what works.
import { readFile } from 'node:fs/promises';

/** Every `openclaw …` command in fenced blocks or inline code of `text`, in document order. */
export function openclawCommands(text) {
  const found = [];
  let fenced = false;
  for (const line of text.split('\n')) {
    if (line.trim().startsWith('```')) { fenced = !fenced; continue; }
    if (fenced && line.trim().startsWith('openclaw ')) found.push(line.trim());
    else if (!fenced) for (const match of line.matchAll(/`(openclaw [^`]+)`/g)) found.push(match[1]);
  }
  return found;
}

/** The first documented command starting with `prefix`, with placeholders replaced; unresolved ones throw. */
export function documented(commands, prefix, replacements = {}) {
  const command = commands.find((candidate) => candidate.startsWith(prefix));
  if (!command) throw new Error(`the docs no longer contain a "${prefix}" command`);
  let result = command;
  for (const [placeholder, value] of Object.entries(replacements)) result = result.split(placeholder).join(value);
  const left = /kubik-xxxxxx|ABCD2345|КОД_С_ЭКРАНА|(?:^|\s)\.\/openclaw-kubik-[\d.]+\.tgz|<[^>]+>/.exec(result);
  if (left) throw new Error(`documented command has an unresolved placeholder "${left[0]}": ${command}`);
  return result;
}

/** Buyer commands from START.html first; KIT.ru.md adds operator recovery commands. */
export async function loadInstallDocs(root) {
  const operator = openclawCommands(await readFile(`${root}/docs/KIT.ru.md`, 'utf8'));
  const buyer = await readFile(`${root}/docs/buyer/START.html`, 'utf8');
  const commands = [...buyer.matchAll(/<pre(?: data-copy)?>([\s\S]*?)<\/pre>/g)]
    .flatMap((match) => match[1].split('\n')).filter((line) => line.startsWith('openclaw '))
    .map((line) => line.replace(/\bCODE\b/g, 'ABCD2345'));
  const buyerPrefixes = ['openclaw plugins install ./', 'openclaw channels add ',
    'openclaw channels status ', 'openclaw pairing list ', 'openclaw pairing approve '];
  for (const prefix of buyerPrefixes) {
    if (!commands.some((command) => command.startsWith(prefix))) throw new Error(`START.html lacks ${prefix}`);
  }
  // The buyer path is executed first. Russian operator commands cover advanced recovery cases.
  return [...commands.filter((command) => buyerPrefixes.some((prefix) => command.startsWith(prefix))), ...operator];
}
