const segmenter = new Intl.Segmenter('und', { granularity: 'grapheme' });
const emojiBase = /[\p{Extended_Pictographic}\p{Regional_Indicator}]|[0-9#*]\ufe0f?\u20e3/u;

export function validScreenEmojiSequence(value) {
  if (typeof value !== 'string' || !value.isWellFormed() || Buffer.byteLength(value) > 64 || /\p{Cc}/u.test(value) || !emojiBase.test(value)) return false;
  if (value.startsWith('\u200d') || value.endsWith('\u200d') || value.includes('\u200d\u200d')) return false;
  const segments = segmenter.segment(value)[Symbol.iterator]();
  return !segments.next().done && segments.next().done;
}

export function containsScreenEmoji(text) {
  for (const { segment } of segmenter.segment(text)) if (validScreenEmojiSequence(segment)) return true;
  return false;
}

export function cleanScreenControls(text) {
  return [...segmenter.segment(text)].map(({ segment }) => validScreenEmojiSequence(segment) ? segment :
    segment.replace(/[\u200B-\u200F\u2060]/g, '')).join('');
}

export function truncateScreenText(text, limit = 1000) {
  if (Buffer.byteLength(text) <= limit) return text;
  let cut = '', bytes = 0;
  for (const { segment } of segmenter.segment(text)) {
    const size = Buffer.byteLength(segment);
    if (bytes + size > limit - 3) break;
    cut += segment; bytes += size;
  }
  return `${cut.replace(/\s+\S*$/, '')}…`;
}

export function validScreenEmojiRequest(message) {
  return Object.keys(message).every(key => ['t', 'id', 'seq'].includes(key)) &&
    Number.isInteger(message.id) && message.id > 0 && message.id <= 0xffffffff &&
    Array.isArray(message.seq) && message.seq.length > 0 && message.seq.length <= 4 &&
    message.seq.every(validScreenEmojiSequence) && new Set(message.seq).size === message.seq.length;
}

export function screenEmojiInText(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > 1000) return new Set();
  return new Set([...segmenter.segment(text)].map(value => value.segment).filter(validScreenEmojiSequence));
}
