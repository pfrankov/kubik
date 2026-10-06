const PAGE_SIZE = 4;
const TIMEOUT_MS = 8000;

/** One device owns this control lane. Model changes never overlap an agent turn. */
export class SessionControl {
  #busy = false;
  #changing = false;
  get changingModel() { return this.#changing; }
  constructor(session, control, isWorking) {
    Object.assign(this, { session, control, isWorking });
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
    if (this.#busy) return this.#respondError(base, { code: 'busy' });
    this.#busy = true;
    this.#changing = message.t === 'agent_model';
    let timer;
    let pending;
    try {
      if (!this.control) throw Object.assign(new Error(), { code: 'unsupported' });
      if (message.t === 'agent_model' && this.isWorking()) throw Object.assign(new Error(), { code: 'busy' });
      const request = async () => {
        if (message.t === 'agent_model') {
          await this.control.selectModel(session.device, message.id, message.target);
          if (message.target !== 'agent') {
            await session.engine.refreshCapabilities?.({ agentId: this.control.agentId?.(session.device) });
          }
        }
        return this.control.options(session.device, message.target);
      };
      pending = request().finally(() => { this.#busy = this.#changing = false; });
      const data = await Promise.race([pending, new Promise((_, reject) => {
        timer = setTimeout(() => reject(Object.assign(new Error(), { code: 'timeout' })), TIMEOUT_MS);
      })]);
      if (session.closed) return;
      this.#respond(base, data, message.t === 'agent_model');
      if (message.t === 'agent_model' && message.target !== 'agent') session.sendCapabilities();
    } catch (error) {
      this.#respondError(base, error);
    } finally {
      clearTimeout(timer);
      if (!pending) this.#busy = this.#changing = false;
    }
  }
}
