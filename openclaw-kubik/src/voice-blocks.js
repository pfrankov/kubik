import { parseReply, splitShown } from './speech.js';

function directiveStart(text) {
  let start = text.lastIndexOf('[');
  if (start > 0 && text[start - 1] === '[') start--;
  if (start < 0) return -1;
  const suffix = text.slice(start).replace(/\s/g, '').toLowerCase();
  return ['[[show]]', '[[/show]]'].some(tag => suffix !== tag && tag.startsWith(suffix)) ? start : -1;
}

// Block coalescing may split a screen-only delimiter. Do not insert a newline
// inside that delimiter, in either the accumulated answer or streamed speech.
export function joinVoiceBlock(answer, text) {
  return `${answer}${answer && directiveStart(answer) < 0 ? '\n' : ''}${text}`;
}

// A dispatcher block can end inside code or markup. Keep that small, bounded
// suffix until its closing delimiter arrives, rather than reading code aloud.
function completeMarkup(text) {
  const plain = text.replace(/```[\s\S]*?```|~~~[\s\S]*?~~~/g, '');
  if (plain.includes('```') || plain.includes('~~~') || (plain.match(/`/g)?.length ?? 0) % 2) return false;
  if (/<(?:\/?[a-zA-Z!][^>]*|\/?)$/.test(plain)) return false;
  const depth = { '[':0, '(':0 }, close = { ']':'[', ')':'(' };
  for (const char of plain) {
    if (char in depth) depth[char]++;
    else if (char in close) depth[close[char]] = Math.max(0, depth[close[char]] - 1);
  }
  return Object.values(depth).every(value => value === 0);
}

export class VoiceBlocks {
  #pending = '';
  #inShow = false;
  #carry = null;
  #directive = '';
  push(text, final = false) {
    const raw = this.#directive + text;
    const start = final ? -1 : directiveStart(raw);
    this.#directive = start < 0 ? '' : raw.slice(start);
    const block = splitShown(start < 0 ? raw : raw.slice(0, start), { inShow:this.#inShow });
    this.#inShow = block.inShow;
    this.#pending += `${this.#pending && block.speech ? '\n' : ''}${block.speech}`;
    if (!final && !completeMarkup(this.#pending)) return [];
    const parsed = parseReply(this.#pending, { carry:this.#carry });
    this.#carry = parsed.pendingEmotion;
    this.#pending = '';
    return parsed.segments;
  }
}
