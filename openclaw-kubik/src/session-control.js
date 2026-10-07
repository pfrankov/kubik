const PAGE_SIZE = 4;
const TIMEOUT_MS = 8000;
const activeByOwner = new WeakMap();

function operationsFor(owner) {
  let operations = activeByOwner.get(owner);
  if (!operations) { operations = new Map(); activeByOwner.set(owner, operations); }
  return operations;
}

/** Serialize control work per control owner and device, including across connection replacement. */
export class SessionControl {
  #operations;
  #deviceId;
  get changingModel() { return this.#operations.get(this.#deviceId)?.changing ?? false; }
  constructor(session, control, isWorking) {
    Object.assign(this, { session, control, isWorking });
    const owner = control && (typeof control === 'object' || typeof control === 'function') ? control : session;
    this.#operations = operationsFor(owner);
    this.#deviceId = session.device.id;
  }
  #respond(base, data, preserveCursor) {
    const models = data.models ?? [];
    const cursor = preserveCursor ? base.cursor : Math.min(base.cursor, Math.max(0, Math.ceil(models.length / PAGE_SIZE) - 1));
    this.session.send({ ...base, cursor, total: models.length, model: data.model ?? '',
      models: models.slice(cursor * PAGE_SIZE, (cursor + 1) * PAGE_SIZE), stt: data.stt, tts: data.tts });
  }
  #respondError(base, error) {
    const code = ['busy', 'timeout', 'unsupported', 'unavailable', 'invalid_model', 'denied'].includes(error?.code)
      ? error.code : 'unavailable';
    if (!this.session.closed) this.session.send({ ...base, total: 0, error: code });
  }
  async handle(message) {
    const { session } = this;
    const base = { t: 'agent_options', rid: message.rid, cursor: message.cursor ?? 0, target: message.target };
    if (this.#operations.has(this.#deviceId)) return this.#respondError(base, { code: 'busy' });
    const operation = { changing: message.t === 'agent_model' };
    this.#operations.set(this.#deviceId, operation);
    let timer;
    let pending;
    let timedOut = false;
    try {
      if (!this.control) throw Object.assign(new Error(), { code: 'unsupported' });
      if (message.t === 'agent_model' && this.isWorking()) throw Object.assign(new Error(), { code: 'busy' });
      const request = async () => {
        if (message.t === 'agent_model') {
          await this.control.selectModel(session.device, message.id, message.target);
          if (message.target !== 'agent') {
            await session.engine.refreshCapabilities?.();
          }
        }
        const data = await this.control.options(session.device, message.target);
        if (!timedOut && !session.closed) this.#respond(base, data, message.t === 'agent_model');
        if (message.t === 'agent_model' && message.target !== 'agent') {
          // The operation can outlive its UI request or connection. Keep the
          // catalog-before-capabilities order, but publish to the current device.
          const current = session.current();
          if (current && current.engine === session.engine) current.sendCapabilities();
        }
      };
      pending = request().finally(() => {
        if (this.#operations.get(this.#deviceId) === operation) this.#operations.delete(this.#deviceId);
      });
      await Promise.race([pending, new Promise((_, reject) => {
        timer = setTimeout(() => {
          timedOut = true;
          reject(Object.assign(new Error(), { code: 'timeout' }));
        }, TIMEOUT_MS);
      })]);
    } catch (error) {
      this.#respondError(base, error);
    } finally {
      clearTimeout(timer);
      if (!pending && this.#operations.get(this.#deviceId) === operation) this.#operations.delete(this.#deviceId);
    }
  }
}
