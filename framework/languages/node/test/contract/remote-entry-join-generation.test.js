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

test('Entry Spot join waits for discovery instead of using an admitted peer G', async () => {
  const {
    decodeActorJoin28
  } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
  const {
    encodeStatefulReply
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const observers = new Set();
  const submissions = [];
  let descriptorGeneration;
  const raw = {
    setServiceIngress() {},
    peerLifecycleGeneration: () => descriptorGeneration,
    topology: { admittedPeer: () => ({ descriptor: { lifecycleGeneration: 11n } }) },
    observePeerRoute: () => ({ ready: true, lifecycleGeneration: descriptorGeneration }),
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
          spot: { spotId: 'target', generation: descriptorGeneration },
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
    const pending = stateful.joinActorEntrySpot(actor.ref, 'target', payload, 1000);
    payload.payload.fill(0);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(submissions.length, 0);
    descriptorGeneration = 12n;
    for (const observer of observers) observer('target');
    const result = await pending.promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.equal(submissions.length, 1);
    assert.equal(submissions[0].targetSpot.targetNodeGeneration, 12n);
    assert.equal(Buffer.from(submissions[0].payload.payload).toString(), '{"seat":1}');
  } finally {
    stateful.close();
  }
});

test('Entry descriptor G is retained by admission and the submitted frame', async () => {
  const {
    decodeActorJoin28
  } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
  const {
    encodeStatefulReply
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const generations = [];
  const frames = [];
  const stateful = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      peerLifecycleGeneration: () => 11n,
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
            spot: { spotId: 'target', generation: 11n },
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
    const result = await stateful.joinActorEntrySpot(actor.ref, 'target', undefined, 1000).promise;
    assert.equal(result.terminalResult, RequestResult.Ok);
    assert.deepEqual(generations, [11n]);
    assert.equal(frames.length, 1);
    assert.equal(frames[0].entry, true);
    assert.equal(frames[0].targetSpot.targetNodeGeneration, 11n);
  } finally {
    stateful.close();
  }
});
