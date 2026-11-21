'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const {
  ServiceMailbox
} = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
const {
  ZLinkMeshDispatchPump
} = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const {
  releaseApplicationJobPermitBeforeHandler
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const {
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const { fixture, deferred } = require('./fixtures/raw-mesh-close');

function originals(claim) {
  return claim.record === undefined ? claim.records : [claim.record];
}
function enqueue(mailbox, owner, id) {
  const record = { owner, domain: 'application', parts: [Buffer.from([id])] };
  assert.equal(mailbox.tryEnqueue(record), true);
  return record;
}
for (const owner of ['node:local', 'channel:orders']) {
  test(`${owner} takes one head and retains ready while prior claims remain`, () => {
    const mailbox = new ServiceMailbox();
    try {
      const expected = Array.from({ length: 8 }, (_, id) => enqueue(mailbox, owner, id));
      const claims = [];
      for (const record of expected) {
        const claim = mailbox.tryClaim('application', 64, 1024);
        assert.ok(claim, 'unreceived residue remains ready before prior release');
        assert.deepEqual(originals(claim), [record], 'claim owns exactly the FIFO head');
        claims.push(claim);
      }
      assert.equal(mailbox.tryClaim('application', 64, 1024), undefined);
      const next = enqueue(mailbox, owner, 9);
      const successor = mailbox.tryClaim('application', 64, 1024);
      assert.deepEqual(originals(successor), [next]);
      for (const claim of claims.reverse()) mailbox.release(claim);
      mailbox.release(successor);
      assert.equal(mailbox.pendingMessages('application'), 0);
      assert.equal(mailbox.pendingBytes('application'), 0);
      assert.equal(mailbox.tryClaim('application', 64, 1024), undefined);
    } finally {
      mailbox.close();
    }
  });
  test(`${owner} enqueue and old release preserve a successor ready reservation`, () => {
    const mailbox = new ServiceMailbox();
    try {
      enqueue(mailbox, owner, 0);
      const first = mailbox.tryClaim('application', 64, 1024);
      const secondRecord = enqueue(mailbox, owner, 1);
      const second = mailbox.tryClaim('application', 64, 1024);
      assert.ok(second, 'enqueue publishes ready while the first claim is held');
      const thirdRecord = enqueue(mailbox, owner, 2);
      mailbox.release(first);
      mailbox.release(first);
      const third = mailbox.tryClaim('application', 64, 1024);
      assert.deepEqual(originals(second), [secondRecord]);
      assert.deepEqual(originals(third), [thirdRecord]);
      mailbox.release(third);
      mailbox.release(second);
      assert.equal(mailbox.tryClaim('application', 64, 1024), undefined);
    } finally {
      mailbox.close();
    }
  });
}

for (const namespace of ['node', 'channel'])
  for (const pair of ['send-send', 'request-request', 'send-request']) {
    test(`${namespace} ${pair} suspended handler permits another logical worker and shutdown waits for terminal`, async () => {
      const f = fixture();
      await f.backend.pumpTask;
      const queue = f.runtime.applicationJobQueue;
      const resume = deferred();
      const terminal = deferred();
      const finishTerminal = deferred();
      let clock = 0,
        nextWorker = 0;
      const workerIds = new WeakMap();
      let currentWorker;
      const createReadyBatch = f.backend.createReadyBatch.bind(f.backend);
      const drainReady = f.backend.drainReady.bind(f.backend);
      f.backend.createReadyBatch = (...args) => {
        const batch = createReadyBatch(...args);
        workerIds.set(batch, ++nextWorker);
        return batch;
      };
      f.backend.drainReady = (domain, batch, ...args) => {
        currentWorker = workerIds.get(batch);
        return drainReady(domain, batch, ...args);
      };
      const entries = new Map();
      const replies = [];
      const errors = [];

      f.runtime.reply = (record, payload) =>
        replies.push({ correlation: record.correlation, payload });
      const kinds = pair.split('-');
      for (let id = 0; id < 2; id++) {
        const request = kinds[id] === 'request';
        const correlation = BigInt(id + 1);
        const header =
          namespace === 'node'
            ? request
              ? wire.encodeNodeRequestHeader(correlation)
              : wire.encodeNodeSendHeader()
            : request
              ? wire.encodeChannelRequestHeader(correlation, 'orders')
              : wire.encodeChannelSendHeader('orders');
        const permit = await queue.acquire(undefined, 'remote');
        const ingress = ApplicationIngressRecordOwner.create(queue, permit, { close() {} });
        const job = ingress.takeInitial('application');
        ingress.close();
        assert.equal(
          f.runtime.mailbox.tryEnqueue({
            owner: namespace === 'node' ? 'node:local' : 'channel:orders',
            domain: 'application',
            parts: [
              header,
              wire.encodeMultipartApplicationPayload(
                [Buffer.from(String(id))],
                SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
                SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
              )
            ],
            correlation: request ? correlation : undefined,
            applicationJob: job
          }),
          true
        );
      }
      const pump = new ZLinkMeshDispatchPump(f.backend, {
        applicationJobQueue: queue,
        monotonicNowMs: () => clock,
        dispatch(_owner, record) {
          const id = Number(record.parts[0].data().toString());
          releaseApplicationJobPermitBeforeHandler();
          entries.set(id, currentWorker);
          if (id === 0) {
            clock += 10;
            record.onTerminalCompletion = () => {
              terminal.resolve();
              return finishTerminal.promise;
            };
            return resume.promise.then(() => {
              if (kinds[id] === 'request') record.reply([Buffer.from('reply:0')]);
            });
          }
          if (kinds[id] === 'request') record.reply([Buffer.from('reply:1')]);
        },
        reportError(error) {
          errors.push(error);
        }
      });
      try {
        pump.start();
        await turn();
        assert.deepEqual(errors, []);
        assert.equal(entries.size, 2, 'both records begin while the held handler is awaiting');
        assert.notEqual(entries.get(0), entries.get(1), 'successor runs on another logical worker');
        assert.equal(
          queue.snapshot().permitsInUse,
          0n,
          'first-handler boundary returns both permits'
        );
        assert.deepEqual(
          replies.map((reply) => reply.correlation),
          kinds[1] === 'request' ? [2n] : []
        );
        let disposed = false;
        const stopping = pump.dispose().then(() => {
          disposed = true;
        });
        await turn();
        assert.equal(disposed, false, 'shutdown retains the suspended handler');
        resume.resolve();
        await terminal.promise;
        await turn();
        assert.equal(disposed, false, 'shutdown retains pending terminal completion');
        finishTerminal.resolve();
        await stopping;
        assert.deepEqual(
          replies.map((reply) => reply.correlation).sort(),
          kinds
            .map((kind, id) => (kind === 'request' ? BigInt(id + 1) : undefined))
            .filter((value) => value !== undefined)
        );
        for (const reply of replies) {
          const id = Number(reply.correlation) - 1;
          assert.deepEqual(
            reply.payload,
            wire.encodeMultipartApplicationPayload(
              [Buffer.from(`reply:${id}`)],
              SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
              SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
            )
          );
        }
        assert.deepEqual(errors, []);
      } finally {
        resume.resolve();
        finishTerminal.resolve();
        await pump.dispose();
        await f.backend.close();
      }
    });
  }

for (const owner of ['spot:one', 'actor:one\0' + '1']) {
  test(`${owner.replaceAll('\0', '/')} retains the gated FIFO prefix and serial reservation`, () => {
    const mailbox = new ServiceMailbox();
    try {
      const expected = Array.from({ length: 5 }, (_, id) => enqueue(mailbox, owner, id));
      const first = mailbox.tryClaim('application', 3, 1024);
      assert.deepEqual(first.records, expected.slice(0, 3));
      enqueue(mailbox, owner, 5);
      assert.equal(mailbox.tryClaim('application', 3, 1024), undefined);
      assert.equal(mailbox.release(first, first.records.slice(2)), true);
      assert.equal(mailbox.release(first), false);
      const second = mailbox.tryClaim('application', 64, 1024);
      assert.deepEqual(
        second.records.map((record) => record.parts[0][0]),
        [2, 3, 4, 5]
      );
      mailbox.release(second);
      assert.equal(mailbox.pendingMessages('application'), 0);
    } finally {
      mailbox.close();
    }
  });
}

for (const owner of ['node:local', 'channel:orders']) {
  test(`${owner} four logical consumers take 256 originals exactly once across idle and last-take edges`, async () => {
    let publications = 0;
    const mailbox = new ServiceMailbox(() => {
      publications++;
    });
    const expected = Array.from({ length: 256 }, (_, id) => enqueue(mailbox, owner, id));
    const taken = [];
    const consumers = Array.from({ length: 4 }, async () => {
      for (;;) {
        const claim = mailbox.tryClaim('application', 64, 1024);
        if (claim === undefined) return;
        taken.push(claim.record);
        // Leave earlier claims alive through subsequent consumers' takes.
        await Promise.resolve();
        assert.equal(mailbox.release(claim), true);
        assert.equal(mailbox.release(claim), false);
      }
    });
    try {
      await Promise.all(consumers);
      assert.deepEqual(taken, expected, 'FIFO take order and original identity');
      assert.equal(new Set(taken).size, 256);
      for (let id = 0; id < 16; id++) {
        const record = enqueue(mailbox, owner, id);
        const claim = mailbox.tryClaim('application', 64, 1024);
        assert.equal(claim.record, record);
        await Promise.resolve();
        mailbox.release(claim);
      }
      assert.equal(
        publications,
        272,
        'enqueue coalesces each ready stretch; successor publication carries residue'
      );
      assert.equal(mailbox.pendingMessages('application'), 0);
      assert.equal(mailbox.pendingBytes('application'), 0);
    } finally {
      mailbox.close();
    }
  });
  test(`${owner} an unreceived claim returns its original once without cancelling newer work`, () => {
    const mailbox = new ServiceMailbox();
    try {
      const first = enqueue(mailbox, owner, 1);
      const claim = mailbox.tryClaim('application', 64, 1024);
      const next = enqueue(mailbox, owner, 2);
      assert.equal(mailbox.release(claim, [claim.record]), true);
      assert.equal(mailbox.release(claim, [claim.record]), false);
      const restored = mailbox.tryClaim('application', 64, 1024);
      const successor = mailbox.tryClaim('application', 64, 1024);
      assert.equal(restored.record, first);
      assert.equal(successor.record, next);
      mailbox.release(successor);
      mailbox.release(restored);
      assert.equal(mailbox.pendingMessages('application'), 0);
    } finally {
      mailbox.close();
    }
  });
}

for (const owner of ['node:local', 'channel:orders']) {
  test(`${owner} raw claim has a single receive, restores abandonment and reuses the receive buffer`, async () => {
    const f = fixture();
    await f.backend.pumpTask;
    const queue = f.runtime.applicationJobQueue;
    const ready = f.backend.createReadyBatch(1);
    const receive = f.backend.createReceiveBatch(64, 256);
    const closed = [];
    async function submit(id) {
      const permit = await queue.acquire(undefined, 'remote');
      const ingress = ApplicationIngressRecordOwner.create(queue, permit, {
        close() {
          closed.push(id);
        }
      });
      const job = ingress.takeInitial('application');
      ingress.close();
      f.runtime.mailbox.tryEnqueue({
        owner,
        domain: 'application',
        applicationJob: job,
        parts: [
          owner.startsWith('node:')
            ? wire.encodeNodeSendHeader()
            : wire.encodeChannelSendHeader('orders'),
          wire.encodeMultipartApplicationPayload(
            [Buffer.from(String(id))],
            SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
            SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
          )
        ]
      });
    }
    function take() {
      ready.reset();
      assert.equal(f.backend.drainReady(1, ready).records.length, 1);
      return ready.takeClaim(0);
    }
    try {
      await submit(0);
      await submit(1);
      const abandoned = take();
      abandoned.release();
      abandoned.release();
      assert.equal(abandoned.recvBatch(receive).ok, false);
      let buffer;
      for (const id of [0, 1]) {
        const claim = take();
        receive.reset();
        const received = claim.recvBatch(receive);
        assert.equal(received.records.length, 1);
        if (buffer !== undefined)
          assert.equal(received.records, buffer, 'worker receive array is reused');
        buffer = received.records;
        const record = received.records[0];
        assert.equal(record.parts[0].data().toString(), String(id));
        assert.equal(claim.recvBatch(receive).ok, false, 'same original cannot be received twice');
        claim.release();
        for (const part of record.parts) part.close();
        record.releaseRetainedIngress();
      }
      assert.deepEqual(closed, [0, 1]);
      assert.equal(queue.snapshot().permitsInUse, 0n);
      ready.reset();
      assert.equal(f.backend.drainReady(1, ready).records.length, 0);
    } finally {
      ready.close();
      receive.close();
      await f.backend.close();
    }
  });
}
