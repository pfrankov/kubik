import { out } from './protocol.js';

/** Batch/live-STT route, kept separate from the native voice transport. */
export async function runRecordedTurn({ session, turn, clearTurn, settleIdle, setState, openStream, feed, endStream }) {
    const stale = () => session.closed || turn.epoch !== session.epoch;
    const startedAt = Date.now();
    let transcript;
    try {
      transcript = await session.engine.commit();
    } catch (error) {
      clearTurn(turn);
      if (stale()) return;
      session.log(`kubik: ${session.device.id} transcription failed: ${error?.message ?? error}`);
      session.send(out.error('stt_failed'));
      settleIdle();
      return;
    }
    clearTurn(turn);
    if (stale()) { session.log(`kubik: ${session.device.id} transcript of superseded turn ${turn.id} dropped`); return; }
    if (!transcript) {
      session.send(out.error('stt_empty'));
      settleIdle();
      return;
    }
    session.log(`kubik: ${session.device.id} turn ${turn.id} transcribed in ${Date.now() - startedAt} ms (${transcript.length} chars)`);
    setState('thinking');
    const stream = openStream('reply');
    // Where a reply's wait goes: button released -> transcript -> first reply text -> first audio.
    stream.timing = { turnId: turn.id, released: startedAt, transcribed: Date.now() };
    let delivered = 0;
    try {
      await session.dispatch({
        transcript, turnId: turn.id,
        isCurrent: () => !stale() && !stream.cancelled,
        onAgentError: (error) => {
          if (stale()) return;
          if (error?.message) session.log(`kubik: ${session.device.id} agent turn failed: ${error.message}`);
          session.send(out.error('agent_failed'));
        },
        deliver: async (text) => {
          delivered++;
          stream.timing.text ??= Date.now();
          if (stale() || stream.cancelled) {
            session.log(`kubik: ${session.device.id} reply block for superseded turn ${turn.id} not spoken`);
            return false;
          }
          return feed(stream, text);
        },
      });
    } catch (error) {
      session.log(`kubik: ${session.device.id} agent dispatch failed: ${error?.message ?? error}`);
      if (!stale()) session.send(out.error('agent_failed'));
    } finally {
      if (!delivered) session.log(`kubik: ${session.device.id} agent produced no reply for turn ${turn.id}`);
      endStream(stream);
    }
  }
