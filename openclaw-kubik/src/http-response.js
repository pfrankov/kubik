/** Bounded JSON from a Fetch Response, including chunked bodies without Content-Length. */
export async function readJsonBounded(response, limit, service) {
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > 2 * 1024 * 1024) {
    throw new RangeError('JSON response limit must be between 1 byte and 2 MiB');
  }
  const oversized = () => new Error(`${service} response exceeded the configured size limit`);
  const stated = Number(response.headers.get('content-length'));
  if (Number.isFinite(stated) && stated > limit) {
    await response.body?.cancel();
    throw oversized();
  }
  if (!response.body) throw new SyntaxError(`${service} returned an empty response`);
  const reader = response.body.getReader();
  const chunks = [];
  let length = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > limit) { await reader.cancel(); throw oversized(); }
      chunks.push(Buffer.from(value));
    }
  } finally { reader.releaseLock(); }
  return JSON.parse(Buffer.concat(chunks, length).toString('utf8'));
}
