// Host side of Kubik LAN discovery (docs/protocol.md): the same broadcast the device sends over Wi-Fi.
import { createSocket } from 'node:dgram';
import { DEVICE_PATH, DISCOVERY_REQUEST, LAN_PORT } from '../../openclaw-kubik/src/protocol.js';

const TRIES = 3;
const TRY_INTERVAL_MS = 1000;

function parseReply(message) {
  try {
    const reply = JSON.parse(message.toString('utf8'));
    return reply?.t === 'kubik' && reply.v === 5 && Number.isInteger(reply.port) && reply.port > 0 && reply.port < 65536 ? reply : null;
  } catch { return null; }
}

/**
 * Broadcasts up to 3 requests 1 s apart and resolves `wss://<source ip>:<port>/kubik/v1` of the first valid reply.
 * `address`/`port` default to 255.255.255.255:18790 (tests use loopback).
 */
export function discoverServer({ address = '255.255.255.255', port = LAN_PORT } = {}) {
  return new Promise((resolve, reject) => {
    const socket = createSocket({ type: 'udp4' });
    let tries = 0, timer = null, done = false;
    const finish = (error, url) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      socket.close();
      if (error) reject(error); else resolve(url);
    };
    const ask = () => {
      if (tries++ >= TRIES) { finish(new Error('no Kubik server answered LAN discovery')); return; }
      socket.send(DISCOVERY_REQUEST, port, address, () => {});
      timer = setTimeout(ask, TRY_INTERVAL_MS);
    };
    socket.on('message', (message, peer) => {
      const reply = parseReply(message);
      if (reply) finish(null, `wss://${peer.address}:${reply.port}${DEVICE_PATH}`);
    });
    socket.once('error', (error) => finish(error));
    socket.bind(0, () => { socket.setBroadcast(true); ask(); });
  });
}
