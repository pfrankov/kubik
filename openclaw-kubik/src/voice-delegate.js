import { parseReply, splitShown } from './speech.js';
import { VoiceBlocks, joinVoiceBlock } from './voice-blocks.js';
export const MAX_VOICE_TEXT = 8192;

/** Accepted tool work stays in the agent history; stale voice delivery is discarded. */
export async function delegateVoice({ dispatch, transcript, turnId, port, isCurrent, onSpeech, onBlock, onPhase = () => {} }) {
  let answer = '', chunks = 0, failed = false;
  const speechBlocks = onSpeech ? new VoiceBlocks() : null;
  onPhase('agent-start');
  await dispatch({ transcript, turnId, isCurrent, onAgentError: () => { failed = true; },
    deliver: async (text) => {
      if (++chunks > 512 || typeof text !== 'string' || answer.length + text.length + 1 > MAX_VOICE_TEXT)
        throw new Error('Agent reply limit');
      answer = joinVoiceBlock(answer, text);
      onPhase('agent-block');
      if (isCurrent()) onBlock?.(answer);
      if (onSpeech && isCurrent()) {
        onSpeech(speechBlocks.push(text));
      }
      return true;
    } });
  onPhase('agent-done');
  if (failed || !answer.trim()) throw new Error('Agent produced no voice reply');
  if (onSpeech && isCurrent()) onSpeech(speechBlocks.push('', true));
  const { speech, shown } = splitShown(answer);
  const { segments } = parseReply(speech);
  const display = shown.join('\n');
  if (isCurrent()) {
    port.reply(answer, display);
    if (!onSpeech) for (const segment of segments) if (segment.emotion) port.emotion?.(segment.emotion);
  }
  return { text: segments.map(segment => segment.text).join(' '), answer, shown: display };
}
