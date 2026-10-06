import { readFileSync } from 'node:fs';

const page = readFileSync(new URL('./setup.html', import.meta.url));
const tess = readFileSync(new URL('../../firmware/main/portal_tess.js', import.meta.url));
const style = readFileSync(new URL('../../firmware/main/portal.css', import.meta.url));
const MAX_BODY_SIZE = 4096;
const REQUEST_TIMEOUT_MS = 5000;

function sendReply(res, status, body, type = 'application/json; charset=utf-8') {
  if (res.writableEnded) return;
  res.writeHead(status, {
    'content-type': type,
    'cache-control': 'no-store',
    'x-content-type-options': 'nosniff',
    'x-frame-options': 'DENY',
    'content-security-policy': "default-src 'none'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'",
  });
  res.end(typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body));
}

function validateRequest(req, hosts, pending) {
  if (!hosts.has(req.headers.host) || (req.headers.origin && req.headers.origin !== `http://${req.headers.host}`)) {
    return { status: 403, body: { ok: false, error: 'Local origin required' } };
  }
  if (req.method === 'GET' && ['/', '/tess.js', '/setup.css'].includes(req.url)) return null;
  if (req.method !== 'POST' || !['/config', '/usb-route'].includes(req.url)) {
    return { status: 404, body: { ok: false } };
  }
  if (!/^application\/json(?:\s*;|$)/i.test(req.headers['content-type'] || '')) {
    return { status: 415, body: { ok: false, error: 'JSON required' } };
  }
  if (pending) return { status: 429, body: { ok: false, error: 'Device busy' } };
  return null;
}

function readRequestBody(req, reply) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    const timer = setTimeout(() => finish(reject, new Error('Request timeout')), REQUEST_TIMEOUT_MS);

    function finish(callback, value) {
      clearTimeout(timer);
      callback(value);
    }

    function collect(chunk) {
      size += chunk.length;
      if (size > MAX_BODY_SIZE) {
        reply(413, { ok: false, error: 'Body too large' });
        finish(reject, new Error('Body too large'));
        return;
      }
      chunks.push(chunk);
    }

    req.on('data', collect);
    req.on('end', () => finish(resolve, Buffer.concat(chunks).toString('utf8')));
    req.on('error', (error) => finish(reject, error));
    req.on('aborted', () => finish(reject, new Error('Request aborted')));
  });
}

function parseJsonBody(body) {
  try { return { command: JSON.parse(body) }; }
  catch { return { error: 'Invalid JSON' }; }
}

function commandError(command, pathname) {
  if (!command || typeof command !== 'object' || Array.isArray(command)) return 'Object required';
  if (pathname === '/usb-route' && typeof command.enabled !== 'boolean') return 'Boolean enabled required';
  return null;
}

// Local-only browser control. A hostile web origin cannot read or change settings.
export function createSetupHandler({ config, route, port = 18791 }) {
  let pending = false;
  const hosts = new Set([`127.0.0.1:${port}`, `localhost:${port}`]);

  return async function handleSetupRequest(req, res) {
    const rejection = validateRequest(req, hosts, pending);
    if (rejection) return sendReply(res, rejection.status, rejection.body);
    if (req.url === '/setup.css') return sendReply(res, 200, style, 'text/css; charset=utf-8');
    if (req.method === 'GET') return req.url === '/tess.js'
      ? sendReply(res, 200, tess, 'text/javascript; charset=utf-8')
      : sendReply(res, 200, page, 'text/html; charset=utf-8');

    pending = true;
    try {
      const body = await readRequestBody(req, (status, result) => sendReply(res, status, result));
      const parsed = parseJsonBody(body);
      if (parsed.error) return sendReply(res, 400, { ok: false, error: parsed.error });

      const error = commandError(parsed.command, req.url);
      if (error) return sendReply(res, 400, { ok: false, error });
      const result = req.url === '/usb-route' ? await route(parsed.command) : await config(parsed.command);
      return sendReply(res, 200, result);
    } catch {
      sendReply(res, 504, { ok: false, error: 'Device did not reply' });
    } finally {
      pending = false;
    }
  };
}
