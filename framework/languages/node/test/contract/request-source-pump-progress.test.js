const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { fixture, deferred } = require('./fixtures/raw-mesh-close');
const {
  ZLinkMeshDispatchPump
} = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const statefulWire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const constants = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

for (const complete of [false, true]) {
  test(
    `pending source Store read preserves control progress and record ownership complete=${complete}`,
    { timeout: 5000 },
    async () => {
      const f = fixture();
      const storeRead = deferred();
      const entered = deferred();
      let reads = 0;
      let signal;
      const resolver = new ZLinkRequestSourceResolver({
        listLiveMeshNodes: (_mesh, stop) => {
          reads++;
          signal = stop;
          entered.resolve();
          return storeRead.promise;
        },
        remainingOwnerTokenLeaseMs: async () => 1000
      });
      f.backend.setRequestSourceAcceptance((rid, generation, storageKey, stop) =>
        resolver.accept('close-mesh', rid, generation, storageKey, stop)
      );
      const actor = f.backend.stateful.createActor('source-probe');
      const localGeneration = f.backend.stateful.nodeGeneration;
      const peer = { descriptor: { lifecycleGeneration: 5n }, liveness: { connectionId: 1n } };
      f.runtime.topology.peer = () => peer;
      f.runtime.topology.admittedPeer = () => peer;
      f.runtime.liveness.recordReceived = () => {};
      let requestClosed = 0;
      let controlClosed = 0;
      const parts = [
        statefulWire.encodeActorHeader(
          'actorRequest',
          {
            actor: actor.ref,
            targetNodeGeneration: localGeneration,
            authorityOwnerGeneration: actor.authorityOwnerGeneration,
            ownerLeaseGeneration: localGeneration
          },
          1n
        ),
        wire.encodeMultipartApplicationPayload(
          [Buffer.from('header'), Buffer.from('{}')],
          constants.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
          constants.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
        )
      ];
      f.records.push(
        {
          sourceRid: 'sender',
          sourceRoute: Buffer.from('sender'),
          routeGeneration: 1n,
          requestSeq: 1n,
          parts,
          close() {
            requestClosed++;
          }
        },
        {
          sourceRid: 'other-peer',
          sourceRoute: Buffer.from('other-peer'),
          routeGeneration: 2n,
          parts: [wire.encodeReject(1)],
          close() {
            controlClosed++;
          }
        }
      );
      const errors = [];
      let dispatched = 0;
      const pump = new ZLinkMeshDispatchPump(f.backend, {
        applicationJobQueue: f.runtime.applicationJobQueue,
        dispatch(_owner, record) {
          dispatched++;
          assert.equal(record.requestSource.ownerId, 'owner');
          assert.equal(record.messageFollowOrigin.requestSource, record.requestSource);
          assert.equal(requestClosed, 0);
        },
        reportError(error) {
          errors.push(error);
        }
      });
      pump.start();
      try {
        f.readable(true, false);
        await turn();
        assert.deepEqual(errors, []);
        assert.equal(reads, 1, 'the record must reach source preparation');
        await entered.promise;
        await turn();
        assert.equal(
          controlClosed,
          1,
          'another peer control must finish while the Store read is held'
        );
        assert.equal(requestClosed, 0, 'the pending record still owns its payload and permit');
        assert.equal(dispatched, 0);
        if (complete) {
          storeRead.resolve([
            { rid: 'sender', lifecycleGeneration: 5n, ownerId: 'owner', leaseGeneration: 1n }
          ]);
          await turn();
          assert.equal(dispatched, 1);
        }
        await pump.dispose();
        await f.backend.close();
        assert.equal(signal?.aborted, true);
        assert.equal(requestClosed, 1);
        assert.equal(f.runtime.applicationJobQueue.snapshot().permitsInUse, 0n);
        assert.equal(dispatched, complete ? 1 : 0);
        assert.deepEqual(errors, []);
        storeRead.resolve([
          { rid: 'sender', lifecycleGeneration: 5n, ownerId: 'owner', leaseGeneration: 1n }
        ]);
        await turn();
        assert.equal(reads, 1);
      } finally {
        storeRead.resolve([]);
        await pump.dispose();
        await f.backend.close();
      }
    }
  );
}
