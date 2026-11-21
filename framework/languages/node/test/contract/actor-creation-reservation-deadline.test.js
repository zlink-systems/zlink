'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist');
const {
  ZLinkActorPlacementCoordinator
} = require('../../packages/framework/dist/runtime/host/actor-placement-coordinator');

for (const expire of [true, false]) {
  test(`Actor reservation distinguishes ${expire ? 'the deadline' : 'caller cancellation'}`, async () => {
    const caller = new AbortController();
    const callerCancellation = new DOMException('Caller cancelled', 'AbortError');
    let reservationAttempts = 0;
    const coordinator = new ZLinkActorPlacementCoordinator({
      target: async () => ({
        meshName: 'play',
        nodeRid: 'target',
        nodeGeneration: 1n,
        entrySpotId: 'entry',
        owner: { ownerId: 'owner', leaseGeneration: 1n },
        isLocal: false,
        sourceNodeRid: 'source',
        sourceNodeGeneration: 1n
      }),
      store: {
        async reserve(_request, signal) {
          reservationAttempts += 1;
          if (!expire) caller.abort(callerCancellation);
          if (signal.aborted) throw signal.reason;
          return await new Promise((_, reject) => {
            signal.addEventListener('abort', () => reject(signal.reason), { once: true });
          });
        }
      }
    });

    await assert.rejects(
      () =>
        coordinator.create(
          'player-1',
          'player',
          false,
          'play',
          Buffer.from('{}'),
          expire ? 20 : 5000,
          caller.signal
        ),
      (failure) =>
        expire
          ? failure instanceof ZLinkFrameworkException &&
            failure.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
          : failure === callerCancellation
    );
    assert.equal(reservationAttempts, 1);
  });
}
