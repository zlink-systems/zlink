const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkMeshDispatchPump
} = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const {
  ReadyDomain,
  ReceiveKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  releaseApplicationJobPermitBeforeHandler
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
async function until(check, label) {
  for (let i = 0; i < 100; i++) {
    if (check()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.fail(`${label} was not observed`);
}
for (const ownerKind of ['Node', 'Channel']) {
  for (const firstKind of ['Send', 'Request']) {
    for (const secondKind of ['Send', 'Request']) {
      for (const batch of ['same', 'next', 'failure']) {
        test(`mesh ${ownerKind} ${firstKind}->${secondKind} ${batch} batch retains each terminal without gating starts`, async () => {
          const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
          const release = deferred();
          const started = [];
          const completed = [];
          const terminals = [];
          const errors = [];
          const failure = new Error('first mesh handler rejected');
          const records = [];
          for (const [index, kind] of [firstKind, secondKind].entries()) {
            const permit = await queue.acquire(undefined, 'remote');
            permit.markApplicationQueued();
            const record = {
              id: index + 1,
              kind: ReceiveKind[ownerKind + kind],
              applicationJobPermit: permit,
              closes: 0,
              releases: 0,
              replies: 0,
              parts: [
                {
                  data() {
                    return Buffer.alloc(0);
                  },
                  close() {
                    record.closes++;
                  }
                }
              ],
              reply() {
                assert.equal(record.closes, 0);
                assert.equal(record.releases, 0);
                record.replies++;
              },
              releaseRetainedIngress() {
                record.releases++;
              },
              onTerminalCompletion() {
                terminals.push(record.id);
              }
            };
            records.push(record);
          }
          let ready;
          let claimed = false;
          let claimsReleased = 0;
          const pending = batch === 'next' ? [[records[0]]] : [[...records]];
          const owner = { ordinaryIngressPreAdmitted: true };
          const node = {
            setReadyHandler(handler) {
              ready = handler;
            },
            createReadyBatch() {
              return {
                reset() {},
                close() {},
                takeClaim() {
                  const received = pending.shift();
                  claimed = true;
                  return {
                    recvBatch() {
                      return { ok: true, records: received };
                    },
                    release() {
                      claimed = false;
                      claimsReleased++;
                      if (pending.length) ready(ReadyDomain.Application);
                    }
                  };
                }
              };
            },
            createReceiveBatch() {
              return { reset() {}, close() {} };
            },
            drainReady() {
              return {
                ok: !claimed && pending.length > 0,
                records: !claimed && pending.length ? [owner] : [],
                hasResidue: false
              };
            }
          };
          const pump = new ZLinkMeshDispatchPump(node, {
            applicationJobQueue: queue,
            async dispatch(_owner, record) {
              releaseApplicationJobPermitBeforeHandler();
              started.push(record.id);
              if (record.id === 1) {
                if (batch === 'next') pending.push([records[1]]);
                await release.promise;
                assert.equal(record.closes, 0);
                assert.equal(record.releases, 0);
                if (batch === 'failure') {
                  completed.push(record.id);
                  throw failure;
                }
              }
              if (record.kind === ReceiveKind[ownerKind + 'Request']) record.reply();
              completed.push(record.id);
            },
            reportError(error) {
              errors.push(error);
            }
          });
          try {
            pump.start();
            ready(ReadyDomain.Application);
            await until(() => terminals.includes(2), 'second terminal before first release');
            assert.deepEqual(started, [1, 2]);
            assert.deepEqual(completed, [2]);
            assert.deepEqual(terminals, [2]);
            assert.equal(records[0].closes, 0);
            assert.equal(records[0].releases, 0);
            assert.equal(records[0].replies, 0);
            assert.equal(records[1].replies, secondKind === 'Request' ? 1 : 0);
            assert.equal(queue.snapshot().permitsInUse, 0n);
            let disposed = false;
            const stopping = pump.dispose().then(() => {
              disposed = true;
            });
            await new Promise((resolve) => setImmediate(resolve));
            assert.equal(disposed, false);
            release.resolve();
            await stopping;
            assert.deepEqual(completed, [2, 1]);
            assert.deepEqual(terminals, batch === 'failure' ? [2] : [2, 1]);
            assert.deepEqual(
              records.map((record) => record.closes),
              [1, 1]
            );
            assert.deepEqual(
              records.map((record) => record.releases),
              [1, 1]
            );
            assert.equal(
              records[0].replies,
              firstKind === 'Request' && batch !== 'failure' ? 1 : 0
            );
            assert.equal(claimsReleased, batch === 'next' ? 2 : 1);
            assert.deepEqual(errors, batch === 'failure' ? [failure] : []);
          } finally {
            release.resolve();
            await pump.dispose();
          }
        });
      }
    }
  }
}

test('mesh synchronous dispatch leaves async terminal cleanup owned until dispose', async () => {
  const release = deferred();
  const started = [];
  const terminal = [];
  const released = [];
  let claimed = false;
  let ready;
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const records = [];
  for (const id of [1, 2]) {
    const permit = await queue.acquire(undefined, 'remote');
    permit.markApplicationQueued();
    records.push({
      id,
      kind: ReceiveKind.NodeSend,
      parts: [],
      applicationJobPermit: permit,
      async onTerminalCompletion() {
        if (id === 1) await release.promise;
        terminal.push(id);
      },
      releaseRetainedIngress() {
        released.push(id);
      }
    });
  }
  const node = {
    setReadyHandler(handler) {
      ready = handler;
    },
    createReadyBatch() {
      return {
        reset() {},
        close() {},
        takeClaim() {
          claimed = true;
          return {
            recvBatch() {
              return { ok: true, records };
            },
            release() {}
          };
        }
      };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: !claimed,
        records: claimed ? [] : [{ ordinaryIngressPreAdmitted: true }],
        hasResidue: false
      };
    }
  };
  const pump = new ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    dispatch(_owner, record) {
      started.push(record.id);
    }
  });
  try {
    pump.start();
    ready(ReadyDomain.Application);
    await until(() => released.includes(2), 'second async terminal before first release');
    assert.deepEqual(started, [1, 2]);
    assert.deepEqual(terminal, [2]);
    assert.deepEqual(released, [2]);
    assert.equal(queue.snapshot().permitsInUse, 1n);
    let disposed = false;
    const stopping = pump.dispose().then(() => {
      disposed = true;
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(disposed, false);
    release.resolve();
    await stopping;
    assert.deepEqual(terminal, [2, 1]);
    assert.deepEqual(released, [2, 1]);
    assert.equal(queue.snapshot().permitsInUse, 0n);
  } finally {
    release.resolve();
    await pump.dispose();
  }
});
