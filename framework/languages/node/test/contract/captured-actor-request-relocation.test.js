'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { inspect } = require('node:util');
const framework = require('../../packages/framework/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  actorId,
  sourceRid,
  targetRid,
  destinationRid,
  timeout,
  actorReplyMutation,
  startCapturedActorRequestRelocation
} = require('./fixtures/captured-actor-request-relocation');
const deferred = require('./helpers/deferred.cjs');

for (const requester of ['source', 'independent host']) {
  test(
    `captured Actor request returns terminal to the original requester (${requester}) across Deferred Join`,
    { timeout: 30_000 },
    async (t) => {
      const terminalWrite = deferred();
      const releaseWrite = deferred();
      const terminalStored = deferred();
      const requesterRid = requester === 'source' ? sourceRid : destinationRid;

      class ReplyTerminalGate extends ZLinkInMemoryProviderLocationStore {
        async write(request, signal) {
          const mutation = actorReplyMutation(request);
          if (mutation?.row.state === 'terminalReceived') {
            terminalWrite.resolve(mutation);
            await releaseWrite.promise;
            const result = await super.write(request, signal);
            if (result.kind === 'applied') terminalStored.resolve();
            return result;
          }
          return super.write(request, signal);
        }
      }

      const locations = new ReplyTerminalGate();
      t.after(() => releaseWrite.resolve());
      const { client, query, request, relocate, releaseCapture, deferredJoin } =
        await startCapturedActorRequestRelocation(t, locations, requester === 'independent host');
      const { moving, capturing } = relocate();
      await capturing;
      const reply = client.requestToActor(actorId, request).timeout(timeout).submit(t.signal);
      void reply.catch(() => {});
      await reply.admitted;
      releaseCapture.resolve();
      const [, response] = await Promise.all([
        moving.then((first) =>
          assert.equal(
            first.outcome,
            framework.ZLinkFrameworkRelocationOutcome.Relocated,
            inspect(first)
          )
        ),
        reply
      ]);
      assert.deepEqual(response, { result: 'scheduled', nodeRid: targetRid });

      const { row, key } = await terminalWrite.promise;
      assert.equal(row.source.nodeRid, Buffer.from(requesterRid).toString('hex'));
      const completion = await deferredJoin.promise;
      assert.equal(completion.status, 'accepted');
      assert.equal(String(completion.actor.nodeRid), destinationRid);
      assert.equal(String((await query.findActorLocation(actorId)).nodeRid), destinationRid);

      releaseWrite.resolve();
      await terminalStored.promise;
      const stored = await locations.read(key);
      assert.equal(stored.kind, 'found');
      assert.deepEqual(JSON.parse(Buffer.from(stored.value.bytes).toString('utf8')), row);
    }
  );
}
