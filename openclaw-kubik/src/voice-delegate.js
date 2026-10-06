import { parseReply, splitShown } from './speech.js';
export const MAX_VOICE_TEXT = 8192;

/** Accepted tool work stays in the agent history; stale voice delivery is discarded. */
export async function delegateVoice({ dispatch, transcript, turnId, port, isCurrent }) {
  let answer = '', chunks = 0, failed = false;
  await dispatch({ transcript, turnId, isCurrent, onAgentError: () => { failed = true; },
    deliver: async (text) => {
      if (++chunks > 512 || typeof text !== 'string' || answer.length + text.length + 1 > MAX_VOICE_TEXT)
        throw new Error('Agent reply limit');
      answer += `${answer ? '\n' : ''}${text}`;
      return true;
    } });
  if (failed || !answer.trim()) throw new Error('Agent produced no voice reply');
  const { speech, shown } = splitShown(answer);
  const { segments } = parseReply(speech);
  const display = shown.join('\n');
  if (isCurrent()) {
    port.reply(answer, display);
    for (const segment of segments) if (segment.emotion) port.emotion?.(segment.emotion);
  }
  return { text: segments.map(segment => segment.text).join(' '), answer, shown: display };
}
