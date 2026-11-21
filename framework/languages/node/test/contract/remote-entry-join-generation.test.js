'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  requestDurableService
} = require('../../packages/framework/dist/runtime/foundation/durable-service-request');
const {
  OperationRegistry
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

test('a restarted Entry Spot target rejects old G with zero submissions', async () => {
  const operations = new OperationRegistry();
  const pending = operations.register(1000);
  let submissions = 0;
  let generation = 11n;
  let changed = () => {};
  const port = {
    observePeerRoute: () => ({
      ready: false,
      lifecycleGeneration: generation,
      expectation: { lifecycleGeneration: generation }
    }),
    observePeerRouteChanged: (listener) => {
      changed = listener;
      return () => {};
    },
    observePeerConnectionIntentRemoved: () => () => {},
    requestService: async () => {
      submissions++;
      return [];
    }
  };
  try {
    const result = requestDurableService(
      port,
      'target',
      [],
      pending,
      'actorJoin',
      undefined,
      undefined,
      undefined,
      11n
    );
    const checked = assert.rejects(
      result,
      (error) =>
        internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RouteNotConnected
    );
    generation = 12n;
    changed('target');
    operations.expire(performance.now() + 1001, performance.now() + 2000);
    await checked;
    assert.equal(submissions, 0);
  } finally {
    operations.close();
  }
});

test('selected Entry route waits for its peer and preserves descriptor ID and lease', async () => {
  const {
    decodeActorJoin28
  } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
  const {
    encodeStatefulReply
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const observers = new Set();
  const submissions = [];
  let peerReady = false;
  const target = {
    spot: { spotId: 'target-entry-uuid', generation: 12n },
    targetNodeRid: 'target',
    targetNodeGeneration: 12n,
    authorityOwnerGeneration: 12n,
    ownerLeaseGeneration: 37n
  };
  const raw = {
    setServiceIngress() {},
    topology: { admittedPeer: () => ({ descriptor: { lifecycleGeneration: 11n } }) },
    observePeerRoute: () => ({ ready: peerReady, lifecycleGeneration: 12n }),
    observePeerRouteChanged: (listener) => {
      observers.add(listener);
      return () => observers.delete(listener);
    },
    observePeerConnectionIntentRemoved: () => () => {},
    requestService: async (_target, parts) => {
      const join = decodeActorJoin28(parts);
      submissions.push(join);
      return [
        encodeStatefulReply(join.correlation, RequestResult.Ok, 0, {
          kind: 'actorJoin',
          joinResult: 0,
          spot: { spotId: 'target-entry-uuid', generation: 12n },
          membershipEpoch: 2n
        })
      ];
    }
  };
  const stateful = new ServiceStatefulRuntime(raw, 'source', 7n);
  try {
    const actor = stateful.createActor('waiting-entry-actor');
    const payload = {
      packetName: 'Join',
      contentType: 'application/json',
      payload: Buffer.from('{"seat":1}')
    };
    const pending = stateful.joinActorEntrySpot(actor.ref, target, payload, 1000);
    payload.payload.fill(0);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(submissions.length, 0);
    peerReady = true;
    for (const observer of observers) observer('target');
    const result = await pending.promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.equal(submissions.length, 1);
    assert.equal(submissions[0].targetSpot.id, 'target-entry-uuid');
    assert.equal(submissions[0].targetSpot.targetNodeGeneration, 12n);
    assert.equal(submissions[0].targetSpot.expectedOwnerLeaseGeneration, 37n);
    assert.equal(Buffer.from(submissions[0].payload.payload).toString(), '{"seat":1}');
  } finally {
    stateful.close();
  }
});

test('selected Entry descriptor route is retained by admission and the submitted frame', async () => {
  const {
    decodeActorJoin28
  } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
  const {
    encodeStatefulReply
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const generations = [];
  const frames = [];
  const target = {
    spot: { spotId: 'target-entry-uuid', generation: 11n },
    targetNodeRid: 'target',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 11n,
    ownerLeaseGeneration: 37n
  };
  const stateful = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      topology: { admittedPeer: () => ({ descriptor: { lifecycleGeneration: 10n } }) },
      observePeerRoute: (_target, generation) => {
        generations.push(generation);
        return { ready: true, lifecycleGeneration: 11n };
      },
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {},
      requestService: async (_target, parts) => {
        const join = decodeActorJoin28(parts);
        frames.push(join);
        return [
          encodeStatefulReply(join.correlation, RequestResult.Ok, 0, {
            kind: 'actorJoin',
            joinResult: 0,
            spot: { spotId: 'target-entry-uuid', generation: 11n },
            membershipEpoch: 2n
          })
        ];
      }
    },
    'source',
    7n
  );
  try {
    const actor = stateful.createActor('remote-entry-actor');
    const result = await stateful.joinActorEntrySpot(actor.ref, target, undefined, 1000).promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.deepEqual(generations, [11n]);
    assert.equal(frames.length, 1);
    assert.equal(frames[0].entry, true);
    assert.equal(frames[0].targetSpot.id, 'target-entry-uuid');
    assert.equal(frames[0].targetSpot.targetNodeGeneration, 11n);
    assert.equal(frames[0].targetSpot.expectedOwnerLeaseGeneration, 37n);
  } finally {
    stateful.close();
  }
});

test('Entry actorJoin keeps the selected descriptor ID and owner lease in its route fence', async () => {
  const {
    decodeActorJoin28
  } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
  const {
    encodeStatefulReply
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const selected = {
    spot: { spotId: 'target-entry-uuid', generation: 11n },
    targetNodeRid: 'target',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 11n,
    ownerLeaseGeneration: 37n
  };
  const frames = [];
  const stateful = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      peerLifecycleGeneration: () => 11n,
      topology: { admittedPeer: () => ({ descriptor: { lifecycleGeneration: 11n } }) },
      observePeerRoute: () => ({ ready: true, lifecycleGeneration: 11n }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {},
      requestService: async (_target, parts) => {
        const join = decodeActorJoin28(parts);
        frames.push(join);
        return [
          encodeStatefulReply(join.correlation, RequestResult.Ok, 0, {
            kind: 'actorJoin',
            joinResult: 0,
            spot: { spotId: 'target-entry-uuid', generation: 11n },
            membershipEpoch: 2n
          })
        ];
      }
    },
    'source',
    7n
  );
  try {
    const actor = stateful.createActor('descriptor-entry-actor');
    const result = await stateful.joinActorEntrySpot(actor.ref, selected, undefined, 1000).promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.equal(frames[0].entry, true);
    assert.deepEqual(
      {
        spotId: frames[0].targetSpot.id,
        ownerLeaseGeneration: frames[0].targetSpot.expectedOwnerLeaseGeneration
      },
      { spotId: 'target-entry-uuid', ownerLeaseGeneration: 37n }
    );
  } finally {
    stateful.close();
  }
});

test('Entry receiver registers the descriptor ID and checks its current owner lease', () => {
  const target = new ServiceStatefulRuntime(
    { setServiceIngress() {} },
    'target',
    7n,
    undefined,
    undefined,
    'target-entry-uuid'
  );
  target.setSessionOwnerDescriptors({
    local: () => ({ ownerId: 'target-owner', leaseGeneration: 37n }),
    peer: async () => undefined
  });
  const route = {
    spot: { spotId: 'target-entry-uuid', generation: 7n },
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 7n,
    ownerLeaseGeneration: 37n
  };
  try {
    assert.equal(target.entrySpot().ref.spotId, 'target-entry-uuid');
    assert.equal(target.validateDirectSpotFence(route).kind, 'entry');
    assert.throws(() =>
      target.validateDirectSpotFence({ ...route, ownerLeaseGeneration: 38n })
    );
  } finally {
    target.close();
  }
});
