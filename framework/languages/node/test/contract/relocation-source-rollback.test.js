const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceRelocationObjectCaptureOwner
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-object-owner');

for (const cleanupFails of [false, true]) {
  test(`capture failure restores every started seal when rollback fails: ${cleanupFails}`, async () => {
    const events = [];
    const failure = new Error('participant seal failed after starting');
    const rollbackFailure = new Error('participant abort failed');
    const unit = (name, objectKind) => ({
      authorityKey: name,
      objectKind,
      stableType: 'test',
      objectGeneration: 1n,
      authorityOwnerGeneration: 1n,
      async seal() {
        events.push(`seal:${name}`);
        return { boundSessionState: Buffer.alloc(0), queuedMessages: [], timers: [] };
      },
      async captureApplicationState() {
        events.push(`capture:${name}`);
        return Buffer.alloc(0);
      },
      commitSeal() {
        events.push(`commit:${name}`);
      },
      abortSeal() {
        events.push(`abort:${name}`);
      }
    });
    const spot = unit('spot', 'user_spot');
    const first = unit('first', 'actor');
    const failed = unit('failed', 'actor');
    const unstarted = unit('unstarted', 'actor');
    failed.seal = () => {
      events.push('seal:failed');
      throw failure;
    };
    first.abortSeal = () => {
      events.push('abort:first');
      if (cleanupFails) throw rollbackFailure;
    };
    const actors = [first, failed, unstarted];
    const outcome = await new ServiceRelocationObjectCaptureOwner()
      .captureUserSpotAggregate(
        'rollback',
        1n,
        spot,
        actors,
        actors.map((actor) => ({
          actorKey: actor.authorityKey,
          spotKey: spot.authorityKey,
          spotObjectGeneration: 1n,
          membershipEpoch: 1n
        }))
      )
      .then(
        () => null,
        (error) => error
      );
    const errors = (error) =>
      error instanceof AggregateError ? error.errors.flatMap(errors) : [error];
    assert.ok(errors(outcome).includes(failure), 'preserve the initiating failure');
    if (cleanupFails)
      assert.ok(errors(outcome).includes(rollbackFailure), 'preserve rollback failures');
    assert.deepEqual(events, [
      'seal:spot',
      'seal:first',
      'seal:failed',
      'abort:failed',
      'abort:first',
      'abort:spot'
    ]);
  });
}
