const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');

for (const failedStage of ['handoff', 'leave', 'both']) {
  test(`source retirement maintenance commits once after ${failedStage} failure`, async () => {
    const failure = new Error(`failed ${failedStage}`);
    const events = [];
    const actor = { context: { actorId: 'maintenance-retirement' } };
    const state = {
      spotId: 'source',
      nativeActorRef: { actorId: actor.context.actorId, generation: 9n },
      beginMove() {},
      endMove() {
        events.push('ended');
      }
    };
    const runtime = new ZLinkActorTransferRuntime({
      actorHandoff: {
        isActive() {
          return false;
        },
        begin() {},
        snapshot() {
          return [];
        },
        complete() {
          events.push('handoff');
          if (failedStage !== 'leave') throw failure;
        }
      },
      spotManager: () => ({
        async beginActorTransfer() {},
        async commitActorLeaveAfterTransfer() {
          events.push('leave');
          if (failedStage !== 'handoff') throw failure;
        }
      })
    });
    const prepared = await runtime.prepareMaintenanceSession(actor, state);
    await assert.rejects(prepared.commit({}, {}, {}), (error) => {
      if (failedStage === 'both') {
        assert.ok(error instanceof AggregateError);
        assert.deepEqual(error.errors, [failure, failure]);
      } else assert.strictEqual(error, failure);
      return true;
    });
    await prepared.commit({}, {}, {});
    assert.deepEqual(events, ['handoff', 'leave', 'ended']);
  });
}
