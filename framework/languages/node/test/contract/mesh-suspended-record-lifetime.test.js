'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshDispatchPump } = require('../../packages/framework/dist/runtime/backend');
const { ReceiveKind, ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { releaseApplicationJobPermitBeforeHandler } = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const { ZLinkSpotSerialTurnExecutor, ZLinkSpotSerialExecutor, ZLinkUserSpotExecutionMode } = require('../../packages/framework/dist/internal');

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

for (const kind of ['SpotSend', 'SpotRequest', 'ActorSend', 'ActorRequest']) {
  test(`${kind} missing pre-admitted permit releases the unstarted record exactly once`, async () => {
    const reported = deferred();
    const events = [];
    let ready;
    let claimed = false;
    const record = {
      kind: ReceiveKind[kind],
      parts: [{ data() { return Buffer.alloc(0); }, close() { events.push('close'); } }],
      releaseRetainedIngress() { events.push('release'); }
    };
    const node = {
      setReadyHandler(handler) { ready = handler; },
      createReadyBatch() {
        return { reset() {}, close() {}, takeClaim() {
          return {
            recvBatch() { return { ok: true, records: [record] }; },
            release() { events.push('claim'); }
          };
        } };
      },
      createReceiveBatch() { return { reset() {}, close() {} }; },
      drainReady() {
        if (claimed) return { ok: false, hasResidue: false, records: [] };
        claimed = true;
        return { ok: true, hasResidue: false, records: [{ ordinaryIngressPreAdmitted: true }] };
      }
    };
    const pump = new ZLinkMeshDispatchPump(node, {
      applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
      dispatch() { assert.fail('an unadmitted record cannot start'); },
      reportError(error) { reported.resolve(error); }
    });
    try {
      pump.start();
      ready(ReadyDomain.Application);
      assert.match((await reported.promise).message, /lost its Application Job Queue permit/);
    } finally {
      await pump.dispose();
    }
    assert.deepEqual(events, ['close', 'release', 'claim']);
  });

  for (const outcome of ['success', 'failure', 'terminal']) {
    test(`${kind} suspended ${outcome} retains its record across batch reuse and shutdown`, async () => {
      const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
      const entered = deferred();
      const resume = deferred();
      const reused = deferred();
      const terminalEntered = deferred();
      const finishTerminal = deferred();
      const errors = [];
      const failure = new Error('handler failed');
      const events = [];
      const records = [];
      for (const id of [0, 1]) {
        const permit = await queue.acquire(undefined, 'remote');
        permit.markApplicationQueued();
        records.push({
          id, kind: ReceiveKind[kind], applicationJobPermit: permit,
          parts: [{ data() { return Buffer.alloc(0); }, close() { events.push(`close:${id}`); } }],
          releaseRetainedIngress() { events.push(`release:${id}`); },
          onTerminalCompletion() {
            events.push(`terminal:${id}`);
            if (id === 0 && outcome === 'terminal') {
              terminalEntered.resolve();
              return finishTerminal.promise;
            }
          }
        });
      }
      let ready;
      const pending = [records[0]];
      const batchRecords = [];
      let claimsReleased = 0;
      const batches = [];
      const node = {
        setReadyHandler(handler) { ready = handler; },
        createReadyBatch() {
          return {
            reset() {}, close() {},
            takeClaim() {
              const record = pending.shift();
              return {
                recvBatch() {
                  batchRecords.push(record);
                  return { ok: true, records: batchRecords };
                },
                release() { claimsReleased++; }
              };
            }
          };
        },
        createReceiveBatch() {
          const batch = {
            closes: 0,
            reset() { batchRecords.length = 0; },
            close() { this.closes++; batchRecords.length = 0; }
          };
          batches.push(batch);
          return batch;
        },
        drainReady() {
          return { ok: pending.length > 0, hasResidue: false,
            records: pending.length ? [{ ordinaryIngressPreAdmitted: true }] : [] };
        }
      };
      const pump = new ZLinkMeshDispatchPump(node, {
        applicationJobQueue: queue,
        monotonicNowMs: () => 0,
        dispatch(_owner, record) {
          releaseApplicationJobPermitBeforeHandler();
          if (record.id === 1) { reused.resolve(); return; }
          entered.resolve();
          return resume.promise.then(() => {
            assert.ok(!events.includes('close:0'));
            assert.ok(!events.includes('release:0'));
            if (outcome === 'failure') throw failure;
          });
        },
        reportError(error) { errors.push(error); }
      });
      try {
        pump.start();
        ready(ReadyDomain.Application);
        await entered.promise;
        // Let the first claim finish, then wake the same idle worker with a new batch.
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(claimsReleased, 1);
        pending.push(records[1]);
        ready(ReadyDomain.Application);
        await reused.promise;
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(claimsReleased, 2);
        assert.deepEqual(events, ['terminal:1', 'close:1', 'release:1']);
        assert.equal(queue.snapshot().permitsInUse, 0n);
        let disposed = false;
        const stopping = pump.dispose().then(() => { disposed = true; });
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(disposed, false, 'activeDrains must retain the suspended terminal');
        resume.resolve();
        if (outcome === 'terminal') {
          await terminalEntered.promise;
          assert.equal(disposed, false);
          assert.ok(!events.includes('close:0'));
          finishTerminal.resolve();
        }
        await stopping;
        assert.deepEqual(events, ['terminal:1', 'close:1', 'release:1',
          ...(outcome === 'failure' ? [] : ['terminal:0']), 'close:0', 'release:0']);
        assert.deepEqual(errors, outcome === 'failure' ? [failure] : []);
        assert.ok(batches.every(batch => batch.closes === 1));
        assert.equal(claimsReleased, 2);
      } finally {
        resume.resolve();
        finishTerminal.resolve();
        await pump.dispose();
        for (const record of records) record.applicationJobPermit.releaseAfterInternalProcessing();
      }
    });
  }

  test(`${kind} returns its worker while the existing execution gate serializes handlers`, async () => {
    const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
    const serial = new ZLinkSpotSerialTurnExecutor();
    const actors = new ZLinkSpotSerialExecutor(serial, ZLinkUserSpotExecutionMode.PerActor, 'same-spot');
    const started = deferred();
    const release = deferred();
    const secondAdmitted = deferred();
    const completed = deferred();
    const events = [];
    const errors = [];
    let ready;
    const pending = [0, 1];
    const node = {
      setReadyHandler(handler) { ready = handler; },
      createReadyBatch() {
        return { reset() {}, close() {}, takeClaim() {
          const id = pending.shift();
          return { recvBatch() { return { ok: true, records: [
            { id, kind: ReceiveKind[kind], parts: [] }
          ] }; }, release() {} };
        } };
      },
      createReceiveBatch() { return { reset() {}, close() {} }; },
      drainReady() {
        return { ok: pending.length > 0, hasResidue: false, records: pending.length ? [{}] : [] };
      }
    };
    const pump = new ZLinkMeshDispatchPump(node, {
      applicationJobQueue: queue,
      dispatch(_owner, record) {
        if (record.id === 1) secondAdmitted.resolve();
        const handler = async () => {
          releaseApplicationJobPermitBeforeHandler();
          events.push(`start:${record.id}`);
          if (record.id === 0) { started.resolve(); await release.promise; }
          events.push(`end:${record.id}`);
          if (record.id === 1) completed.resolve();
        };
        return kind.startsWith('Actor') ? actors.executeActor('same-actor', handler) : serial.execute(handler);
      },
      reportError(error) { errors.push(error); }
    });
    try {
      pump.start();
      ready(ReadyDomain.Application);
      await started.promise;
      await secondAdmitted.promise;
      assert.deepEqual(events, ['start:0']);
      release.resolve();
      await completed.promise;
      assert.deepEqual(events, ['start:0', 'end:0', 'start:1', 'end:1']);
      assert.deepEqual(errors, []);
    } finally {
      release.resolve();
      await pump.dispose();
    }
    assert.equal(queue.snapshot().permitsInUse, 0n);
  });
}
