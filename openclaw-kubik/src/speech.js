// Turns agent reply text into speakable segments: emotion tags become events,
// everything a voice cannot say (markdown, URLs, code, emoji) is removed.

export const EMOTIONS = Object.freeze(['neutral', 'happy', 'joy', 'love', 'sad', 'angry', 'surprised', 'confused',
  'sleepy', 'thinking', 'wink', 'shy', 'proud']);
const EMOTION_SET = new Set(EMOTIONS);
// Any [[...]] directive. Allowed emotion names become events; everything else (e.g. [[reply_to_current]]) is dropped.
const TAG_RE = /\[\[\s*([^\[\]\n]{0,64}?)\s*\]\]/g;

const EMOJI_RE = /[\p{Extended_Pictographic}\u{1F1E6}-\u{1F1FF}\u{1F3FB}-\u{1F3FF}\u200D\uFE0E\uFE0F\u20E3]/gu;
const URL_RE = /\b(?:https?:\/\/|ftp:\/\/|www\.)[^\s<>()\]]+/giu;
const EMAIL_RE = /\b[\w.+-]+@[\w-]+\.[\w.-]+\b/gu;

/** Normalises a tag body like " Happy " or "emotion: happy" to an allowed emotion, or null. */
export function emotionOf(tagBody) {
  const name = String(tagBody).trim().toLowerCase().replace(/^(?:emotion|emo|e)\s*[:=]\s*/, '');
  return EMOTION_SET.has(name) ? name : null;
}

/**
 * Makes one piece of reply text safe to read aloud. Returns '' when nothing speakable remains.
 * Order matters: code and links are handled before generic punctuation cleanup.
 */
export function sanitizeForSpeech(input) {
  let text = String(input ?? '').replace(/\r\n?/g, '\n');
  text = text.replace(TAG_RE, ' ');
  text = text.replace(/```[\s\S]*?(?:```|$)/g, ' ').replace(/~~~[\s\S]*?(?:~~~|$)/g, ' '); // fenced code is not read
  text = text.replace(/<\/?[a-z][^>\n]*>/gi, ' '); // HTML tags
  text = text.replace(/!\[([^\]\n]*)\]\([^)\n]*\)/g, ' '); // images
  text = text.replace(/\[([^\]\n]+)\]\((?:[^)\n]*)\)/g, '$1'); // [label](url) -> label
  text = text.replace(/\[([^\]\n]+)\]\[[^\]\n]*\]/g, '$1'); // reference links
  text = text.replace(/^\s*\[[^\]\n]+\]:\s*\S+.*$/gm, ' '); // link definitions
  text = text.replace(URL_RE, ' ').replace(EMAIL_RE, ' ');
  text = text.replace(/`([^`\n]*)`/g, '$1');
  text = text.replace(EMOJI_RE, '');
  text = text.replace(/:[a-z0-9_+-]{0,29}[a-z][a-z0-9_+-]{0,29}:/g, " "); // :shortcode: emoji
  const lines = [];
  for (let line of text.split('\n')) {
    if (/^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(line)) continue; // table separator
    if (/^\s*([-*_=])\1{2,}\s*$/.test(line)) continue; // horizontal rule
    line = line.replace(/^\s{0,3}#{1,6}\s+/, '') // headings
      .replace(/^\s*(?:>\s*)+/, '') // quotes
      .replace(/^\s*(?:[-*+•·]|\d{1,3}[.)])\s+(?:\[[ xX]\]\s+)?/, ''); // list markers and task boxes
    if (line.includes('|')) line = line.replace(/^\s*\|/, '').replace(/\|\s*$/, '').replace(/\s*\|\s*/g, ', ');
    line = line.replace(/(\*\*|__|~~)(?=\S)([\s\S]*?\S)\1/g, '$2')
      .replace(/(^|[\s(«"])[*_](?=\S)([^*_\n]*?\S)[*_](?=$|[\s).,!?:;»"])/g, '$1$2')
      .replace(/[*#~`^\\]+/g, ' ')
      .replace(/(^|\s)_+|_+(?=\s|$)/g, '$1')
      .replace(/\s+/g, ' ').trim();
    if (!line) continue;
    // A list item or heading without final punctuation still needs a pause when joined.
    lines.push(/[.!?…:;,]$/.test(line) ? line : `${line}.`);
  }
  text = lines.join(' ')
    .replace(/\(\s*\)|\[\s*\]/g, ' ')
    .replace(/\s+([.,!?…:;])/g, '$1')
    .replace(/([.,!?…:;])(?:\s*[.,;:])+(?=\s|$)/g, '$1')
    .replace(/\s{2,}/g, ' ')
    .trim();
  return /[\p{L}\p{N}]/u.test(text) ? text : '';
}

// [[show]] … [[/show]]: text for Kubik's screen, never spoken. A block may stop inside one (`inShow`).
const SHOW_OPEN_RE = /\[\[\s*show\s*\]\]/i;
const SHOW_CLOSE_RE = /\[\[\s*\/\s*show\s*\]\]/i;

/** Splits text into [speech, shown] parts at [[show]] … [[/show]]; `inShow` carries an unclosed block over. */
export function splitShown(input, { inShow = false } = {}) {
  let rest = String(input ?? '');
  let speech = '';
  const shown = [];
  for (;;) {
    if (inShow) {
      const close = rest.match(SHOW_CLOSE_RE);
      shown.push(close ? rest.slice(0, close.index) : rest);
      if (!close) break;
      rest = rest.slice(close.index + close[0].length);
      inShow = false;
    } else {
      const open = rest.match(SHOW_OPEN_RE);
      speech += open ? `${rest.slice(0, open.index)} ` : rest;
      if (!open) break;
      rest = rest.slice(open.index + open[0].length);
      inShow = true;
    }
  }
  return { speech, shown: shown.filter((s) => s.trim()), inShow };
}

const DISPLAY_MAX_BYTES = 1000;

/**
 * Makes reply text fit for Kubik's small screen: markdown markup and emoji go, links and numbers stay, list
 * items become "• " lines. Returns '' when nothing is left; long text is cut at DISPLAY_MAX_BYTES with "…".
 */
export function sanitizeForDisplay(input) {
  let text = String(input ?? '').replace(/\r\n?/g, '\n');
  text = text.replace(TAG_RE, ' ').replace(/```[^\n]*\n?|~~~[^\n]*\n?/g, '');
  text = text.replace(/<\/?[a-z][^>\n]*>/gi, ' ').replace(/!\[([^\]\n]*)\]\([^)\n]*\)/g, '$1')
    .replace(/\[([^\]\n]+)\]\(([^)\s\n]*)\)/g, (m, label, url) => (label === url ? url : `${label} (${url})`));
  text = text.replace(EMOJI_RE, '').replace(/[\u200B-\u200F\u2060]/g, '');
  const lines = [];
  for (let line of text.split('\n')) {
    if (/^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(line)) continue;
    if (/^\s*([-*_=])\1{2,}\s*$/.test(line)) continue;
    line = line.replace(/^\s{0,3}#{1,6}\s+/, '').replace(/^\s*(?:>\s*)+/, '')
      .replace(/^\s*[-*+•·]\s+(?:\[[ xX]\]\s+)?/, '• ');
    if (line.includes('|')) line = line.replace(/^\s*\|/, '').replace(/\|\s*$/, '').replace(/\s*\|\s*/g, ' · ');
    line = line.replace(/(\*\*|__|~~)(?=\S)([\s\S]*?\S)\1/g, '$2').replace(/`([^`\n]*)`/g, '$1')
      .replace(/(^|[\s(«"])[*_](?=\S)([^*_\n]*?\S)[*_](?=$|[\s).,!?:;»"])/g, '$1$2')
      .replace(/[ \t]+/g, ' ').trim();
    if (line || (lines.length && lines.at(-1))) lines.push(line);
  }
  while (lines.length && !lines.at(-1)) lines.pop();
  text = lines.join('\n');
  if (!/[\p{L}\p{N}]/u.test(text)) return '';
  if (Buffer.byteLength(text) > DISPLAY_MAX_BYTES) {
    let cut = text.slice(0, DISPLAY_MAX_BYTES);
    while (Buffer.byteLength(cut) > DISPLAY_MAX_BYTES - 3) cut = cut.slice(0, -1);
    text = `${cut.replace(/\s+\S*$/, '')}…`;
  }
  return text;
}

const FIRST_CHUNK_MIN = 30;
const FIRST_CHUNK_MAX = 160;
const CHUNK_MAX = 400;

/**
 * Cuts speakable text into TTS requests at sentence ends. The first request of a reply is short (its audio
 * comes back soonest); the next ones are longer and are synthesised while the previous one plays.
 */
export function chunkSpeech(text, { first = false } = {}) {
  const sentences = String(text).split(/(?<=[.!?…]["»)]*)\s+/u).map((s) => s.trim()).filter(Boolean);
  const parts = sentences.flatMap((sentence, index) => {
    // A very long first sentence: split at its first comma-like pause in the quick-start window.
    const comma = first && index === 0 && sentence.length > FIRST_CHUNK_MAX
      ? sentence.slice(FIRST_CHUNK_MIN, FIRST_CHUNK_MAX).search(/[,;:—–]\s/u)
      : -1;
    if (comma < 0) return [sentence];
    const headEnd = FIRST_CHUNK_MIN + comma + 1;
    return [sentence.slice(0, headEnd).trim(), sentence.slice(headEnd).trim()].filter(Boolean);
  });
  const state = parts.reduce((result, sentence) => {
    const isFirstChunk = first && result.chunks.length === 0;
    const limit = isFirstChunk ? FIRST_CHUNK_MAX : CHUNK_MAX;
    if (result.current && result.current.length + 1 + sentence.length > limit) {
      result.chunks.push(result.current);
      result.current = '';
    }
    result.current = result.current ? `${result.current} ${sentence}` : sentence;
    if (first && result.chunks.length === 0 && result.current.length >= FIRST_CHUNK_MIN) {
      result.chunks.push(result.current);
      result.current = '';
    }
    return result;
  }, { chunks: [], current: '' });
  return [...state.chunks, ...(state.current ? [state.current] : [])];
}

/**
 * Splits one reply block into [{ emotion, text }] segments. `emotion` is the tag that precedes the text
 * (or `carry` for text before the first tag). A trailing tag with no text after it is returned as
 * `pendingEmotion` so the caller can apply it to the next block of the same reply.
 */
export function parseReply(input, { carry = null } = {}) {
  const raw = String(input ?? '');
  const segments = [];
  let emotion = carry;
  let cursor = 0;
  const push = (chunk) => {
    const text = sanitizeForSpeech(chunk);
    if (!text) return false;
    const last = segments.at(-1);
    // Neighbouring text without an emotion change is one TTS request.
    if (last && (emotion === null || emotion === last.emotion)) last.text = `${last.text} ${text}`;
    else segments.push({ emotion, text });
    emotion = null;
    return true;
  };
  for (const match of raw.matchAll(TAG_RE)) {
    const tagged = emotionOf(match[1]);
    if (!tagged) continue; // unknown directives stay in the text and are removed by the sanitizer
    const before = raw.slice(cursor, match.index);
    cursor = match.index + match[0].length;
    push(before);
    emotion = tagged;
  }
  push(raw.slice(cursor));
  return { segments, pendingEmotion: emotion };
}

/** Plain text for logs/tests: what would be spoken, without events. */
export function speakableText(input) {
  return parseReply(input).segments.map((segment) => segment.text).join(' ');
}
