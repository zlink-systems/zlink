'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const deferred = require('./helpers/deferred.cjs');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/internal');
const { ZLinkAbortError } = require('../../packages/framework/dist/runtime/abort');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');

for (const scenario of ['success', 'failure', 'cancel']) {
  const cancel = scenario === 'cancel';
  const fail = scenario === 'failure';
  test(`Spot context Close join after activation detach (${scenario})`, async (t) => {
    const releaseEntered = deferred();
    const finishRelease = deferred();
    const caller = new AbortController();
    const events = [];
    const closeFailure = new Error('injected shared Close failure');
    class Room {
      onClosing() {
        events.push('OnClosing');
      }
    }

    const manager = new DefaultZLinkSpotManager({
      spotFactories: [Room],
      detachedTaskRunner: new ZLinkRuntimeTaskRunner(
        new ZLinkRuntimeTaskErrorSink(),
        new AbortController().signal
      ),
      beginUserClosingAuthority: async (_mesh, _spotId, _generation, committed) => {
        committed();
        return {
          release: async () => {
            events.push('authority released');
            releaseEntered.resolve();
            await finishRelease.promise;
            if (fail) throw closeFailure;
          }
        };
      }
    });
    const mesh = 'mesh';
    const spotId = 'close-join';
    await manager.getOrCreate(mesh, Room, spotId);
    const activation = manager.resolveRelocationActivation(mesh, spotId);
    const { context } = activation.spot;
    const { serial } = activation;
    const closeAdmission = serial.closeAdmission.bind(serial);
    t.mock.method(serial, 'closeAdmission', () => {
      closeAdmission();
      assert.ok(serial.applicationAdmissionRefusal(false));
      events.push('Spot stopped');
    });

    const owner = context.close();
    try {
      await Promise.race([releaseEntered.promise, owner]);
      assert.deepEqual(events, ['OnClosing', 'authority released']);
      const joined = context.close(caller.signal);
      events.push('join submitted');
      if (cancel) {
        caller.abort();
        events.push('caller aborted');
      }
      const outcome = cancel
        ? assert.rejects(joined, ZLinkAbortError)
        : fail
          ? assert.rejects(joined, closeFailure)
          : joined;
      finishRelease.resolve();
      if (cancel || fail) await outcome;
      else assert.equal(await outcome, true);
    } finally {
      finishRelease.resolve();
      if (fail) await assert.rejects(owner, closeFailure);
      else assert.equal(await owner, true);
      assert.equal(manager.closeOperations.size, 0);
      assert.deepEqual(events, [
        'OnClosing',
        'authority released',
        'join submitted',
        ...(cancel ? ['caller aborted'] : []),
        ...(fail ? [] : ['Spot stopped'])
      ]);
    }
  });
}
