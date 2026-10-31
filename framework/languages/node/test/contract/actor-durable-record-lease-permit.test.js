const { ApplicationIngressRecordOwner } = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorSerialExecutor
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  runWithApplicationJobPermit,
  releaseApplicationJobPermitBeforeHandler
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');

test(
  'an Actor FIFO reacquires a record lease behind unstarted P2 while P1 runs',
  { timeout: 1000 },
  async () => {
    const queue = new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
    );
    const mailbox = new ZLinkActorSerialExecutor('actor', 'spot');
    const events = [];
    let finishP1;
    let startedP1;
    const running = new Promise((resolve) => {
      startedP1 = resolve;
    });
    const finish = new Promise((resolve) => {
      finishP1 = resolve;
    });
    const records = ['P1', 'P2'].map((name) => {
      let permit;
      return {
        preparation: {
          async prepare(signal) {
            permit = await queue.acquire(signal, 'remote');
          },
          cancel() {
            permit?.releaseAfterInternalProcessing();
          }
        },
        operation: (execute) =>
          runWithApplicationJobPermit(permit, () =>
            execute(async () => {
              releaseApplicationJobPermitBeforeHandler();
              events.push(name);
              if (name === 'P1') {
                startedP1();
                await finish;
              }
            })
          )
      };
    });
    const prefix = mailbox.admitDurablePrefix(records);
    await running;
    const owner = ApplicationIngressRecordOwner.create(
      queue,
      await queue.acquire(undefined, 'remote'),
      { close() {} }
    );
    const initial = owner.takeInitial('application');
    initial.markApplicationQueued();
    const q = runWithApplicationJobPermit(initial, () =>
      mailbox.execute(() => {
        releaseApplicationJobPermitBeforeHandler();
        events.push('Q');
      })
    );
    assert.equal(
      queue.snapshot().permitsInUse,
      0n,
      'Q must surrender its initial reservation to the FIFO'
    );
    finishP1();
    await Promise.all([
      ...prefix,
      q.finally(() => {
        initial.close();
        owner.close();
      })
    ]);
    assert.deepEqual(events, ['P1', 'P2', 'Q']);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    await mailbox.close();
  }
);

