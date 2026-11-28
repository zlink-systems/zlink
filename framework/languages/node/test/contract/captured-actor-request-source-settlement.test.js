'use strict';

const assert = require('node:assert/strict');
const { AsyncLocalStorage } = require('node:async_hooks');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const {
  ZLinkActorHandoffCoordinator
} = require('../../packages/framework/dist/runtime/actors/actor-handoff');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  actorId,
  sourceSpotId,
  targetRid,
  destinationRid,
  timeout,
  actorReplyMutation,
  startCapturedActorRequestRelocation
} = require('./fixtures/captured-actor-request-relocation');
const deferred = require('./helpers/deferred.cjs');

test(
  'Actor source settlement after Deferred Join to a subsequent owner',
  { timeout: 30_000 },
  async (t) => {
    const cutover = deferred();
    const releaseCommit = deferred();
    const settlementRead = deferred();
    const releaseSettlement = deferred();
    const releasePending = deferred();
    const terminalStored = deferred();
    t.after(() => {
      releaseCommit.resolve();
      releaseSettlement.resolve();
      releasePending.resolve();
    });

    const locations = new ZLinkInMemoryProviderLocationStore();
    const write = locations.write.bind(locations);
    const rowWrites = [];
    t.mock.method(locations, 'write', async (request, signal) => {
      const mutation = actorReplyMutation(request);
      if (mutation === undefined) return write(request, signal);
      const { row } = mutation;
      rowWrites.push(row.state);
      if (row.state === 'accepted') {
        cutover.resolve();
        await releaseCommit.promise;
      }
      if (row.state === 'pending') await releasePending.promise;
      const result = await write(request, signal);
      if (
        result.kind === 'applied' &&
        ['terminalReceived', 'alreadyTerminal'].includes(row.state)
      ) {
        terminalStored.resolve(mutation);
      }
      return result;
    });

    const { client, query, request, relocate, releaseCapture, deferredJoin } =
      await startCapturedActorRequestRelocation(t, locations, true);
    const relocatingSource = new AsyncLocalStorage();
    const coordinator = ZLinkActorHandoffCoordinator.prototype;
    const snapshot = coordinator.snapshot;
    const capturedPackets = [];
    t.mock.method(coordinator, 'snapshot', function (id) {
      const packets = snapshot.call(this, id);
      if (id === actorId && relocatingSource.getStore()) capturedPackets.push(...packets);
      return packets;
    });
    let cutoverTaken = false;
    const takeRelocationRelay = coordinator.takeRelocationRelay;
    t.mock.method(coordinator, 'takeRelocationRelay', function (id) {
      const packets = takeRelocationRelay.call(this, id);
      if (id === actorId && relocatingSource.getStore()) {
        capturedPackets.push(...packets);
        cutoverTaken = true;
      }
      return packets;
    });

    const sourceSpotKey = encodeAuthorityKey('instance_spot', sourceSpotId);
    const repository = ZLinkLocationStoreRepository.prototype;
    const readAuthority = repository.readAuthority;
    t.mock.method(repository, 'readAuthority', async function (key, signal) {
      if (cutoverTaken && relocatingSource.getStore() && key.value === sourceSpotKey.value) {
        settlementRead.resolve();
        await releaseSettlement.promise;
      }
      return readAuthority.call(this, key, signal);
    });

    const complete = coordinator.complete;
    let settledFence;
    t.mock.method(coordinator, 'complete', function (id, target, ref, results, fence) {
      if (id === actorId && String(target.targetNodeRid) === targetRid) {
        settledFence = fence;
      }
      return complete.call(this, id, target, ref, results, fence);
    });

    const { moving, capturing } = relocatingSource.run(true, relocate);
    void moving.then(releasePending.resolve, releasePending.resolve);
    await capturing;
    const captured = client.requestToActor(actorId, request).timeout(timeout).submit(t.signal);
    const capturedResult = Promise.allSettled([captured]);
    await captured.admitted;
    releaseCapture.resolve();

    // The accepted-row CAS follows cutover and still holds the first source authority.
    await Promise.all([cutover.promise, settlementRead.promise]);
    const tail = client.requestToActor(actorId, request).timeout(timeout).submit(t.signal);
    const tailResult = Promise.allSettled([tail]);
    await tail.admitted;
    releaseCommit.resolve();

    const joined = await deferredJoin.promise;
    assert.equal(joined.status, 'accepted');
    assert.equal(String(joined.actor.nodeRid), destinationRid);
    releaseSettlement.resolve();
    assert.equal((await moving).outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
    assert.equal(String((await query.findActorLocation(actorId)).nodeRid), destinationRid);
    assert.equal(settledFence, null);
    assert.equal(capturedPackets.length, 1);

    await t.test('the original requester receives the captured reply once', async () => {
      const [{ status, value, reason }] = await capturedResult;
      const { row, key } = await terminalStored.promise;
      const stored = await locations.read(key);
      assert.equal(stored.kind, 'found');
      assert.equal(row.operationId, capturedPackets[0].messageFollowContext.operationId);
      assert.equal(row.source.nodeRid, Buffer.from(destinationRid).toString('hex'));
      assert.deepEqual(JSON.parse(Buffer.from(stored.value.bytes).toString('utf8')), row);
      assert.deepEqual(
        { status, value, error: reason?.message, rowWrites },
        {
          status: 'fulfilled',
          value: { result: 'scheduled', nodeRid: targetRid },
          error: undefined,
          rowWrites: ['accepted', 'pending', 'terminalReceived']
        }
      );
    });
    await t.test('the post-boundary request fails with a stale location', async () => {
      const [{ status, reason }] = await tailResult;
      assert.equal(status, 'rejected');
      assert.equal(reason.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      assert.match(reason.message, /is stale after the Message Follow duration/);
    });
  }
);
