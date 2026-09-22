// Stands in for Runtime/ZlinkStreamWebGlConnector.cs while driving the jslib.
//
// It follows the same rules as the C# connector so the test exercises the real
// contract: one static sink that only copies the event, a frame loop that pumps
// the boundary and yields, and user callbacks that run after the jslib call has
// returned.
const MAX_EVENTS_PER_PUMP = 256;

const EVENT_CALL_COMPLETED = 1;
const EVENT_MESSAGE = 2;
const EVENT_ERROR_RECEIVED = 3;
const EVENT_DISCONNECTED = 4;
const EVENT_STATE_CHANGED = 5;
const EVENT_ACTOR_BOUND = 6;
const EVENT_ACTOR_UNBOUND = 7;

class JslibConnector {
  constructor(harness, optionsJson) {
    this.harness = harness;
    this.library = harness.library;
    this.inbox = [];
    this.dispatchQueue = [];
    this.pending = new Map();
    this.observerNames = new Map();
    this.observed = new Set();
    this.handlers = new Map();
    this.received = new Map();
    this.stateChanges = [];
    this.errors = [];
    this.disconnects = [];
    this.actorEvents = [];
    this.actorBoundHandlers = new Set();
    this.actorUnboundHandlers = new Set();
    this.dispatchMode = JSON.parse(optionsJson).dispatchMode ?? 'manual';
    this.nextCallId = 1;
    this.advance = null;
    this.sinkStack = 0;
    this.nestedPumpResults = [];

    this.handle = harness.withString(optionsJson, (pointer) => this.library.ZlinkStreamCreate(pointer));
    if (this.handle === 0) {
      const error = this.takeLastError();
      const failure = new Error(error ? error.message : 'connector creation failed');
      failure.code = error ? error.code : undefined;
      throw failure;
    }

    this.sinkPointer = harness.registerFunction(
      (handle, eventType, id, value, textPointer, bytesPointer, bytesLength) => {
        // Copy only. No user code, no promise, no throw: the pointers are freed
        // as soon as this returns and the JS stack below is an emscripten frame.
        this.sinkStack += 1;
        try {
          this.nestedPumpResults.push(this.library.ZlinkStreamPump(handle, MAX_EVENTS_PER_PUMP));
          this.inbox.push({
            eventType,
            id,
            value,
            text: textPointer ? harness.readString(textPointer) : null,
            bytes: harness.readBytes(bytesPointer, bytesLength)
          });
        } finally {
          this.sinkStack -= 1;
        }
      }
    );
    if (this.library.ZlinkStreamSetEventSink(this.handle, this.sinkPointer) !== 1) {
      throw new Error('event sink registration failed');
    }
  }

  takeLastError() {
    const pointer = this.library.ZlinkStreamTakeLastError();
    if (!pointer) return null;
    try {
      return JSON.parse(this.harness.readString(pointer));
    } finally {
      this.library.ZlinkStreamFreeBuffer(pointer);
    }
  }

  get isConnected() {
    return this.library.ZlinkStreamIsConnected(this.handle) === 1;
  }

  get state() {
    return this.library.ZlinkStreamGetState(this.handle);
  }

  get closeReason() {
    return this.library.ZlinkStreamGetCloseReason(this.handle);
  }

  get diagnosticsLevel() {
    return this.library.ZlinkStreamGetDiagnosticsLevel(this.handle);
  }

  get pendingDispatchCount() {
    return this.dispatchQueue.length;
  }

  connect(timeoutMs) {
    return this.runLifecycle('ZlinkStreamConnect', false, timeoutMs);
  }

  close(timeoutMs) {
    return this.runLifecycle('ZlinkStreamClose', false, timeoutMs);
  }

  async dispatch(timeoutMs) {
    this.startAdvanceIfIdle();
    if (this.advance) await this.advance;
    this.pumpAndTransfer();
    await this.runDispatchQueue();
  }

  async send(payload, call, timeoutMs) {
    const callId = this.nextCallId++;
    const pending = this.registerPending(callId);
    this.harness.withString(JSON.stringify(call), (callPointer) =>
      this.harness.withBytes(payload, (bytes, length) =>
        this.library.ZlinkStreamSend(this.handle, callId, callPointer, bytes, length)));
    await this.drive(callId, pending, true, timeoutMs);
  }

  async request(payload, call, timeoutMs) {
    const callId = this.nextCallId++;
    const pending = this.registerPending(callId);
    this.harness.withString(JSON.stringify(call), (callPointer) =>
      this.harness.withBytes(payload, (bytes, length) =>
        this.library.ZlinkStreamRequest(this.handle, callId, callPointer, bytes, length)));
    await this.drive(callId, pending, true, timeoutMs);
    return pending.result;
  }

  on(name, handler) {
    return this.registerHandler(name, { handler, actorHandle: undefined });
  }

  onActor(actorHandle, name, handler) {
    return this.registerHandler(name, { handler, actorHandle });
  }

  registerHandler(name, registration) {
    this.ensureObserved(name);
    const handlers = this.handlers.get(name) ?? [];
    handlers.push(registration);
    this.handlers.set(name, handlers);
    return {
      dispose: () => {
        const index = handlers.indexOf(registration);
        if (index >= 0) handlers.splice(index, 1);
        if (handlers.length === 0 && this.handlers.get(name) === handlers) {
          this.handlers.delete(name);
        }
      }
    };
  }

  onActorBound(handler) {
    this.actorBoundHandlers.add(handler);
    return { dispose: () => this.actorBoundHandlers.delete(handler) };
  }

  onActorUnbound(handler) {
    this.actorUnboundHandlers.add(handler);
    return { dispose: () => this.actorUnboundHandlers.delete(handler) };
  }

  receivedCount(name) {
    this.ensureObserved(name);
    return (this.received.get(name) ?? []).length;
  }

  async waitFor(name, predicate, timeoutMs) {
    this.ensureObserved(name);
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      if (this.isConnected) this.startAdvanceIfIdle();
      this.pumpAndTransfer();
      const message = this.tryTake(name, predicate);
      if (message) return message;
      if (Date.now() >= deadline) throw new Error(`Timed out waiting for '${name}' stream message.`);
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
  }

  destroy() {
    this.library.ZlinkStreamDestroy(this.handle);
  }

  ensureObserved(name) {
    if (this.observed.has(name)) return;
    const observerId = this.harness.withString(name, (pointer) =>
      this.library.ZlinkStreamObserve(this.handle, pointer));
    this.observed.add(name);
    this.observerNames.set(observerId, name);
  }

  registerPending(callId) {
    const pending = { completed: false, result: null, error: null };
    this.pending.set(callId, pending);
    return pending;
  }

  async runLifecycle(entry, advanceTransport, timeoutMs) {
    const callId = this.nextCallId++;
    const pending = this.registerPending(callId);
    this.library[entry](this.handle, callId);
    await this.drive(callId, pending, advanceTransport, timeoutMs);
  }

  async drive(callId, pending, advanceTransport, timeoutMs) {
    const deadline = Date.now() + (timeoutMs ?? 15000);
    try {
      while (!pending.completed) {
        if (advanceTransport && this.isConnected) this.startAdvanceIfIdle();
        this.pumpAndTransfer();
        if (pending.completed) break;
        if (Date.now() > deadline) throw new Error(`boundary call ${callId} did not complete`);
        await new Promise((resolve) => setTimeout(resolve, 1));
      }

      if (pending.error) {
        const failure = new Error(pending.error.message);
        failure.code = pending.error.code;
        throw failure;
      }
    } finally {
      this.pending.delete(callId);
    }
  }

  startAdvanceIfIdle() {
    if (this.advance && !this.advanceDone) return;
    const callId = this.nextCallId++;
    const pending = this.registerPending(callId);
    this.library.ZlinkStreamDispatch(this.handle, callId);
    this.advanceDone = false;
    this.advance = this.drive(callId, pending, false)
      .catch(() => undefined)
      .finally(() => { this.advanceDone = true; });
  }

  pumpAndTransfer() {
    this.library.ZlinkStreamPump(this.handle, MAX_EVENTS_PER_PUMP);
    while (this.inbox.length > 0) this.transfer(this.inbox.shift());
  }

  transfer(event) {
    switch (event.eventType) {
      case EVENT_CALL_COMPLETED: {
        const pending = this.pending.get(event.id);
        if (!pending) return;
        pending.completed = true;
        if (event.value === 1) {
          pending.result = event.bytes.length > 0 || event.text
            ? { codec: JSON.parse(event.text ?? '{}').codec ?? 0, payload: event.bytes }
            : null;
        } else {
          pending.error = JSON.parse(event.text);
        }

        return;
      }

      case EVENT_MESSAGE: {
        const observed = this.observerNames.get(event.id);
        if (!observed) return;
        const descriptor = JSON.parse(event.text);
        const message = {
          name: descriptor.name ?? observed,
          metadata: descriptor.metadata ?? {},
          actorId: descriptor.actorId ?? undefined,
          payload: { codec: event.value, payload: event.bytes }
        };
        const handlers = this.handlers.get(message.name);
        if (handlers && handlers.length > 0) {
          this.dispatchOrQueue({ kind: 'message', message, actorHandle: descriptor.actorHandle });
          return;
        }

        const history = this.received.get(message.name) ?? [];
        history.push(message);
        this.received.set(message.name, history);
        return;
      }

      case EVENT_ERROR_RECEIVED:
        this.dispatchQueue.push({ kind: 'error', error: JSON.parse(event.text) });
        return;

      case EVENT_DISCONNECTED:
        this.dispatchQueue.push({ kind: 'disconnected', detail: JSON.parse(event.text) });
        return;

      case EVENT_STATE_CHANGED:
        this.dispatchQueue.push({ kind: 'state', change: JSON.parse(event.text) });
        return;

      case EVENT_ACTOR_BOUND:
        this.dispatchOrQueue({ kind: 'actorBound', actor: JSON.parse(event.text) });
        return;

      case EVENT_ACTOR_UNBOUND:
        this.dispatchOrQueue({ kind: 'actorUnbound', actor: JSON.parse(event.text) });
        return;

      default:
    }
  }

  async runDispatchQueue() {
    while (this.dispatchQueue.length > 0) {
      await this.runDispatchItem(this.dispatchQueue.shift());
    }
  }

  dispatchOrQueue(item) {
    if (this.dispatchMode === 'manual') this.dispatchQueue.push(item);
    else void this.runDispatchItem(item);
  }

  async runDispatchItem(item) {
    switch (item.kind) {
      case 'message':
        await this.runMessageHandlers(item);
        break;
      case 'error':
        this.errors.push(item.error);
        break;
      case 'disconnected':
        this.disconnects.push(item.detail);
        break;
      case 'actorBound':
      case 'actorUnbound': {
        const handlers = item.kind === 'actorBound'
          ? this.actorBoundHandlers
          : this.actorUnboundHandlers;
        for (const handler of [...handlers]) handler(item.actor);
        this.actorEvents.push({
          kind: item.kind,
          actorId: item.actor.actorId,
          actorHandle: item.actor.actorHandle
        });
        break;
      }
      default:
        this.stateChanges.push(item.change);
    }
  }

  async runMessageHandlers(item) {
    const handlers = this.handlers.get(item.message.name) ?? [];
    const matching = handlers.filter(
      (registration) => registration.actorHandle === undefined ||
        registration.actorHandle === item.actorHandle
    );
    if (matching.length === 0) {
      const history = this.received.get(item.message.name) ?? [];
      history.push(item.message);
      this.received.set(item.message.name, history);
      return;
    }
    for (const registration of matching) await registration.handler(item.message);
  }

  tryTake(name, predicate) {
    const history = this.received.get(name);
    if (!history) return null;
    for (let index = 0; index < history.length; index += 1) {
      if (predicate && !predicate(history[index])) continue;
      const [message] = history.splice(index, 1);
      if (history.length === 0) this.received.delete(name);
      return message;
    }

    return null;
  }
}

module.exports = { JslibConnector, MAX_EVENTS_PER_PUMP };
