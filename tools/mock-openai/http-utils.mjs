// ---- HTTP helpers ---------------------------------------------------------------------------------------
async function readBody(req, limit = 32 * 1024 * 1024) {
  const chunks = [];
  let size = 0;
  for await (const chunk of req) { size += chunk.length; if (size > limit) throw Object.assign(new Error('too large'), { status: 413 }); chunks.push(chunk); }
  return Buffer.concat(chunks);
}
function json(res, status, body) { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(body)); }
function apiError(res, status, message, code = null) { json(res, status, { error: { message, type: 'invalid_request_error', code } }); }
function parseMultipart(body, contentType) {
  const boundary = /boundary=(?:"([^"]+)"|([^;]+))/i.exec(contentType ?? '');
  if (!boundary) return null;
  const delimiter = Buffer.from(`--${boundary[1] ?? boundary[2]}`);
  const parts = {};
  let start = body.indexOf(delimiter);
  while (start !== -1) {
    const next = body.indexOf(delimiter, start + delimiter.length);
    if (next === -1) break;
    const part = body.subarray(start + delimiter.length + 2, next - 2); // skip CRLF after delimiter and before next
    const split = part.indexOf('\r\n\r\n');
    if (split !== -1) {
      const headers = part.subarray(0, split).toString('utf8');
      const name = /name="([^"]+)"/i.exec(headers)?.[1];
      const content = part.subarray(split + 4);
      if (name) parts[name] = /filename=/i.test(headers) ? content : content.toString('utf8');
    }
    start = next;
  }
  return parts;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export { readBody, json, apiError, parseMultipart, sleep };

