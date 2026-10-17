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
  'an Actor FIFO owns Q behind unstarted P2 and returns Q initial permit while P1 runs',
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
    const initial = await queue.acquire(undefined, 'remote');
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
    await Promise.all([...prefix, q]);
    assert.deepEqual(events, ['P1', 'P2', 'Q']);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    await mailbox.close();
  }
);

for (const phase of ['waiting', 'prepared']) {
  test(
    `withdrawing a ${phase} durable record cleans its waiter and permit once`,
    { timeout: 1000 },
    async () => {
      const queue = new ApplicationJobQueue(
        resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
      );
      const mailbox = new ZLinkActorSerialExecutor('actor', 'spot');
      const blocker = phase === 'waiting' ? await queue.acquire(undefined, 'local') : undefined;
      let withdraw;
      let prepared;
      let attempts = 0;
      let releases = 0;
      const [terminal] = mailbox.admitDurablePrefix([
        {
          operation: async () => assert.fail('a withdrawn record cannot run'),
          workOptions: {
            onWithdrawable: (take) => {
              withdraw = take;
            }
          },
          preparation: {
            async prepare(signal) {
              attempts += 1;
              prepared = await queue.acquire(signal, 'remote');
            },
            cancel() {
              if (prepared !== undefined) {
                prepared.releaseAfterInternalProcessing();
                prepared = undefined;
                releases += 1;
              }
            }
          }
        }
      ]);
      // Stop the consumer at the existing readiness boundary. This permits
      // observing a grant without allowing the handler claim in between.
      const schedule = mailbox.scheduler.scheduleDrain.bind(mailbox.scheduler);
      mailbox.scheduler.scheduleDrain = () => {};
      await new Promise(setImmediate);
      assert.equal(attempts, 1);
      assert.equal(queue.snapshot().capacityWaiters, phase === 'waiting' ? 1n : 0n);
      assert.equal(withdraw(), true);
      assert.equal(withdraw(), false);
      await terminal;
      await new Promise(setImmediate);
      assert.equal(queue.snapshot().capacityWaiters, 0n);
      assert.equal(releases, phase === 'prepared' ? 1 : 0);
      blocker?.releaseAfterInternalProcessing();
      assert.equal(queue.snapshot().permitsInUse, 0n);
      mailbox.scheduler.scheduleDrain = schedule;
      await mailbox.close();
    }
  );
}
