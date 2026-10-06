import { out, parseAudioFrame, parseDeviceMessage, ProtocolError } from './protocol.js';

/** Before the session exists: hello → challenge → auth, then pairing; only `ping` may be answered on the way. */
function handshakeFrame(ws, conn, data, isBinary, on) {
  if (conn.phase === 'closed') return;
  if (isBinary) throw new ProtocolError(conn.phase === 'hello' ? 'hello must be the first frame' : 'binary frame before welcome');
  const message = parseDeviceMessage(data);
  if (conn.phase === 'hello') {
    if (message.t !== 'hello') throw new ProtocolError('hello must be the first frame');
    return on.hello(message);
  }
  if (conn.phase === 'challenge') {
    if (message.t !== 'auth') throw new ProtocolError('auth must follow the challenge');
    return on.auth(message);
  }
  if (message.t !== 'ping') throw new ProtocolError(conn.phase === 'pending' ? 'device is waiting for pairing approval' : 'unexpected frame during authentication');
  ws.send(JSON.stringify(out.pong(message.ts)));
}

/** In a session every frame goes through `conn.frames` (the allow-list guard, see guardSessionFrames). */
function sessionFrame(conn, data, isBinary) {
  if (isBinary) return conn.frames.audio(parseAudioFrame(data));
  const message = parseDeviceMessage(data);
  if (message.t === 'hello') throw new ProtocolError('duplicate hello');
  if (message.t === 'auth') throw new ProtocolError('unexpected auth');
  return conn.frames.message(message);
}

/** Routes the frames of one device socket by connection phase; `on` = { hello, auth, error }. */
export function attachDeviceMessages(ws, conn, on) {
  ws.on('pong', () => { ws.kubikLastSeen = Date.now(); });
  ws.on('message', (data, isBinary) => {
    ws.kubikLastSeen = Date.now();
    try {
      return conn.session ? sessionFrame(conn, data, isBinary) : handshakeFrame(ws, conn, data, isBinary, on);
    } catch (error) { on.error(error); }
  });
}
