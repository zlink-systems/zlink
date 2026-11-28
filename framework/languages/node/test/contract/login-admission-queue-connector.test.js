const assert = require('node:assert/strict');
const test = require('node:test');
const connector = require('../../packages/stream-connector/dist');
const {
  decodeStreamWireHeader,
  encodeStreamWireHeader
} = require('../../packages/stream-wire/dist');
const { ZlinkStreamFrameCodec } = require('./helpers/stream-protocol-codecs');
const fixture = require('../../../../runtime/conformance/login-admission-queue-v1.json');

const enqueue = '$zlink.admission.enqueue';
const queueRequired = '$zlink.admission.queueRequired';
const retryAfter = '$zlink.admission.retryAfterMs';
const ticketKey = '$zlink.admission.ticket';
const kinds = connector.ZlinkStreamMessageKind;
const flags = connector.ZlinkStreamHeaderFlags;
const json = connector.ZlinkStreamCodec.Json;
const wireTicket = fixture.ticketVectors[0].ticket;

function frame(kind, payload, { name = '', seq, metadata = {} } = {}) {
  const values = new Map(Object.entries(metadata));
  return ZlinkStreamFrameCodec.encode(
    encodeStreamWireHeader({
      kind,
      name,
      codec: json,
      requestSeq: seq,
      metadata: values,
      flags:
        (seq === undefined ? 0 : flags.HasRequestSeq) | (values.size === 0 ? 0 : flags.HasMetadata)
    }),
    Buffer.from(JSON.stringify(payload))
  );
}

function useClock(t) {
  let now = 0;
  const timers = new Map();
  t.mock.method(performance, 'now', () => now);
  t.mock.method(Math, 'random', () => 0);
  t.mock.method(globalThis, 'setTimeout', (callback, delay) => {
    const id = {};
    timers.set(id, { at: now + Math.max(1, delay), callback });
    return id;
  });
  t.mock.method(globalThis, 'clearTimeout', (id) => timers.delete(id));
  async function flush() {
    await new Promise(setImmediate);
  }
  return async (target) => {
    for (;;) {
      const next = [...timers]
        .filter(([, timer]) => timer.at <= target)
        .sort((left, right) => left[1].at - right[1].at)[0];
      if (next === undefined) break;
      const [id, timer] = next;
      timers.delete(id);
      now = timer.at;
      timer.callback();
      await flush();
    }
    now = target;
    await flush();
  };
}

test('login admission fixture has 11 connector scenarios', () => {
  assert.equal(fixture.connectorScenarios.length, 11);
});

for (const { name, given, steps } of fixture.connectorScenarios) {
  test(`login admission queue connector fixture: ${name}`, async (t) => {
    const advance = useClock(t);
    const writes = [];
    const inbound = [];
    const callbacks = [];
    const outcomes = new Map();
    const transport = {
      closed: false,
      async write(bytes) {
        const { header, payload } = ZlinkStreamFrameCodec.decode(bytes);
        writes.push({
          ...decodeStreamWireHeader(header),
          payload: JSON.parse(Buffer.from(payload).toString())
        });
      },
      async read() {
        return inbound.shift();
      },
      async close() {
        this.closed = true;
      }
    };
    const instance = connector.zlinkStreamConnectorFactory.create({
      endpoint: 'ws://127.0.0.1:19000',
      transportFactory: {
        async connect() {
          return transport;
        }
      },
      heartbeat: { enabled: false },
      reconnect: { enabled: false },
      maxAdmissionWaitMs: given.maxAdmissionWaitMs
    });
    t.after(() => instance.close());
    instance.onAdmissionQueued?.(() => callbacks.push('OnAdmissionQueued'));
    instance.onAdmissionPosition?.((position, estimatedWaitMs) => {
      callbacks.push('OnAdmissionPosition');
      lastPosition = { position, estimatedWaitMs };
    });
    instance.onAdmissionWaiting(() => callbacks.push('OnAdmissionWaiting'));
    let lastPosition;
    await instance.connect();
    instance.onRequestSending(({ setMetadata }) => setMetadata('source', 'login'));

    const requests = given.requests ?? ['a'];
    for (const id of requests) {
      instance
        .request({ id })
        .packetName(id)
        .timeout(given.originalDeadlineMs ?? 40_000)
        .submitCallback((result) => {
          outcomes.set(id, result.isSuccess ? 'success' : result.error.code);
          callbacks.push('requestComplete');
        });
    }
    const deliver = async (kind, payload, details) => {
      inbound.push(frame(kind, payload, details));
      await instance.dispatch();
      await advance(performance.now());
      await instance.dispatch();
    };
    const lastRequest = (id) => writes.findLast((write) => write.name === id);
    const error = (id, metadata, code = 'unavailable') =>
      deliver(
        kinds.Error,
        {
          code,
          message: 'Authentication admission.'
        },
        { seq: lastRequest(id).requestSeq, metadata }
      );
    const register = (id) => error(id, { [queueRequired]: '1' });
    const enqueueFrames = () => writes.filter(({ name: packet }) => packet === enqueue);
    await advance(0);

    let previousEnqueues = 0;
    if (given.cachedTicket === 'old') {
      const old = fixture.ticketVectors.find(({ name }) => name === 'previous-key').ticket;
      await register('a');
      previousEnqueues = enqueueFrames().length;
      await deliver(
        kinds.Response,
        { ticket: old, expiresInMs: 30_000 },
        {
          seq: enqueueFrames().at(-1).requestSeq
        }
      );
      assert.equal(lastRequest('a').metadata.get(ticketKey), old);
      callbacks.length = 0;
    }
    if (given.firstRejectionMs !== undefined) {
      await advance(given.firstRejectionMs);
      await register('a');
    }
    for (const step of steps) {
      const before = callbacks.length;
      const id = step.request ?? requests[0];
      if (step.atMs !== undefined) await advance(step.atMs);
      switch (step.action) {
        case 'queueRequired':
          await register(id);
          break;
        case 'positionResponse':
        case 'positionSend': {
          const payload = { position: step.position, estimatedWaitMs: step.estimatedWaitMs };
          if (step.action === 'positionResponse') {
            await deliver(kinds.Response, payload, { seq: enqueueFrames().at(-1).requestSeq });
          } else {
            await deliver(kinds.Send, payload, { name: '$zlink.admission.position' });
          }
          if (step.expect.callbacks) assert.deepEqual(lastPosition, payload);
          break;
        }
        case 'ticketSend':
          assert.equal(step.ticket, 'T');
          await deliver(
            kinds.Send,
            { ticket: wireTicket, expiresInMs: step.expiresInMs },
            {
              name: ticketKey
            }
          );
          break;
        case 'stageOneReject':
          await error(id, { [retryAfter]: step.retryAfterMs });
          await advance(performance.now() + Number(step.retryAfterMs));
          assert.equal(lastRequest(id).metadata.get(ticketKey), wireTicket);
          break;
        case 'complete':
          await deliver(kinds.Response, { ok: true }, { seq: lastRequest(id).requestSeq });
          assert.equal(outcomes.get(id), 'success');
          break;
        case 'error':
          await error(id, step.metadata, step.code);
          break;
        case 'enqueueResponse':
          await register('a');
          await register('b');
          await deliver(kinds.Response, step.payload, { seq: enqueueFrames().at(-1).requestSeq });
          break;
        case 'send':
          for (const requestId of requests) await register(requestId);
          await deliver(kinds.Send, step.payload, { name: step.packet });
          if (step.expect.discarded) assert.equal(instance.pendingDispatchCount, 0);
          break;
        case 'silence':
          break;
        case 'wait':
          await instance.dispatch();
          assert.equal(step.atMs, step.expect.deadlineMs);
          assert.equal(outcomes.get('a'), connector.ZlinkStreamErrorCode[step.expect.result]);
          const count = writes.length;
          await advance(step.atMs + 15_000);
          assert.equal(writes.length > count, step.expect.sendAfterDeadline);
          break;
        default:
          assert.fail(`Unknown action: ${step.action}`);
      }
      const expected = step.expect;
      if (expected.enqueueCount !== undefined) {
        assert.equal(enqueueFrames().length - previousEnqueues, expected.enqueueCount);
        assert.deepEqual(enqueueFrames().at(-1).payload, {});
      }
      if (expected.cachedTicket === null) {
        assert.equal(enqueueFrames().at(-1).metadata.has(ticketKey), false);
        await advance(performance.now() + 1);
        assert.equal(writes.filter((write) => write.name === id).length, 2);
      }
      if (expected.queuedCallbacks !== undefined) {
        assert.equal(
          callbacks.filter((callback) => callback === 'OnAdmissionQueued').length,
          expected.queuedCallbacks
        );
      }
      if (expected.callbacks !== undefined)
        assert.deepEqual(callbacks.slice(before), expected.callbacks);
      if (expected.positionCallbacks !== undefined) {
        assert.equal(
          callbacks.slice(before).filter((callback) => callback === 'OnAdmissionPosition').length,
          expected.positionCallbacks
        );
      }
      if (expected.connectionClosed !== undefined)
        assert.equal(transport.closed, expected.connectionClosed);
      for (const requestId of requests) {
        const outcome = expected[requestId];
        if (outcome !== undefined) {
          assert.equal(
            outcomes.get(requestId) ?? 'pending',
            connector.ZlinkStreamErrorCode[outcome] ?? outcome
          );
        }
      }
      if (expected.retriedRequests !== undefined) {
        assert.deepEqual(
          writes
            .filter((write) => write.metadata.get(ticketKey) === wireTicket)
            .map((write) => write.name),
          expected.retriedRequests
        );
        for (const requestId of expected.retriedRequests) {
          const original = writes.find((write) => write.name === requestId);
          const retried = lastRequest(requestId);
          assert.equal(retried.correlationId, original.correlationId);
          assert.notEqual(retried.requestSeq, original.requestSeq);
          assert.deepEqual(retried.payload, original.payload);
          const metadata = new Map(retried.metadata);
          metadata.delete(ticketKey);
          assert.deepEqual(metadata, original.metadata);
        }
      }
    }
  });
}
