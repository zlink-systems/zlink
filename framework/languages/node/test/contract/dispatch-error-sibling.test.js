'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const backend = require('../../packages/framework/dist/runtime/backend');
const {
  ReadyDomain,
  ReceiveKind,
  OperationKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

for (const domain of [ReadyDomain.Infrastructure, ReadyDomain.Application]) {
  test(`record dispatch failure preserves its sibling in domain ${domain}`, async () => {
    const queue = new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2n })
    );
    const closed = [0, 0];
    const released = [0, 0];
    const records = await Promise.all(
      [0, 1].map(async (sequence) => ({
        sequence,
        kind: ReceiveKind.NodeSend,
        operationKind: OperationKind.NodeRequest,
        sourceNodeRid: null,
        parts: [
          {
            data() {
              return Buffer.alloc(0);
            },
            close() {
              closed[sequence]++;
            }
          }
        ],
        applicationJobPermit:
          domain === ReadyDomain.Application ? await queue.acquire() : undefined,
        releaseRetainedIngress() {
          released[sequence]++;
        }
      }))
    );
    let ready;
    let drained = false;
    let received = false;
    let finish;
    const claimFinished = new Promise((resolve) => {
      finish = resolve;
    });
    const failure = new Error('first record failed');
    const errors = [];
    const dispatched = [];
    const node = {
      setReadyHandler(handler) {
        ready = handler;
      },
      createReadyBatch() {
        return {
          reset() {},
          close() {},
          takeClaim() {
            return {
              recvBatch() {
                if (received) return { ok: false, records: [] };
                received = true;
                return { ok: true, records };
              },
              release() {
                finish();
              }
            };
          }
        };
      },
      createReceiveBatch() {
        return { reset() {}, close() {} };
      },
      drainReady() {
        if (drained) return { ok: false, records: [], hasResidue: false };
        drained = true;
        return {
          ok: true,
          records: [{ ordinaryIngressPreAdmitted: domain === ReadyDomain.Application }],
          hasResidue: false
        };
      }
    };
    const pump = new backend.ZLinkMeshDispatchPump(node, {
      applicationJobQueue: queue,
      dispatch(_owner, record) {
        dispatched.push(record.sequence);
        if (record.sequence === 0) throw failure;
      },
      reportError(error) {
        errors.push(error);
      }
    });
    try {
      pump.start();
      ready(domain);
      await claimFinished;
    } finally {
      await pump.dispose();
    }
    assert.deepEqual(dispatched, [0, 1]);
    assert.deepEqual(errors, [failure]);
    assert.deepEqual(closed, [1, 1]);
    assert.deepEqual(released, [1, 1]);
    assert.equal(queue.snapshot().permitsInUse, 0n);
  });
}
