const assert = require('node:assert/strict');
const test = require('node:test');
const { routeChangePeers } = require('./helpers/route-change-peer');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  decodeActorJoin28
} = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const {
  encodeStatefulReply
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const framework = require('../../packages/framework/dist');

test('cross-node Join preserves DeadlineExceeded after replacement admission without redispatching', async () => {
  const f = await routeChangePeers();
  let stateful;
  try {
    await f.until(
      () => f.source.raw.isPeerRouteReady('z-target') && f.target.raw.isPeerRouteReady('a-source'),
      'initial admission'
    );
    let delivered = 0;
    let terminal;
    f.target.raw.setServiceIngress((record) => {
      const request = decodeActorJoin28(record.parts);
      delivered++;
      terminal ??= [encodeStatefulReply(request.correlation, RequestResult.Rejected, 15)];
      f.target.raw.replyService(record, terminal);
      return 'infrastructure';
    });
    stateful = new ServiceStatefulRuntime(f.source.raw, 'a-source', 1n);
    const actor = stateful.createActor('joining');
    const spot = { spotId: 'room', generation: 1n };
    stateful.rememberSpotRoute({
      spot,
      targetNodeRid: 'z-target',
      targetNodeGeneration: 1n,
      authorityOwnerGeneration: 1n,
      ownerLeaseGeneration: 1n,
      storeVersion: 'target-v1'
    });
    await f.replaceBeforeSourceObservation();
    let outcome;
    const join = stateful.joinActor(actor.ref, 'z-target', spot, spot.generation, undefined, 1000);
    const finished = join.promise.then(
      (value) => {
        outcome = value;
      },
      (error) => {
        outcome = { error };
      }
    );
    await f.untilTargetOnly(
      () => f.targetRequests() > 0,
      'new route receives original request before admission'
    );
    assert.equal(
      delivered,
      0,
      'target-only receive did not deliver the request through service ingress'
    );
    await f.until(
      () => f.source.raw.isPeerRouteReady('z-target') && f.target.raw.isPeerRouteReady('a-source'),
      'replacement admission'
    );
    await finished;
    assert.ok(outcome.error instanceof framework.ZLinkFrameworkException);
    assert.equal(outcome.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
    assert.equal(delivered, 0);
    assert.equal(f.targetRequests(), 1);
  } finally {
    stateful?.close();
    await f.close();
  }
});

test('cross-node Join receives one original 106/15 reply when both routes are admitted', async () => {
  const f = await routeChangePeers();
  let stateful;
  try {
    await f.until(
      () => f.source.raw.isPeerRouteReady('z-target') && f.target.raw.isPeerRouteReady('a-source'),
      'initial admission'
    );
    let delivered = 0;
    let terminal;
    f.target.raw.setServiceIngress((record) => {
      const request = decodeActorJoin28(record.parts);
      delivered++;
      terminal ??= [encodeStatefulReply(request.correlation, RequestResult.Rejected, 15)];
      f.target.raw.replyService(record, terminal);
      return 'infrastructure';
    });
    stateful = new ServiceStatefulRuntime(f.source.raw, 'a-source', 1n);
    const actor = stateful.createActor('joining');
    const spot = { spotId: 'room', generation: 1n };
    stateful.rememberSpotRoute({
      spot,
      targetNodeRid: 'z-target',
      targetNodeGeneration: 1n,
      authorityOwnerGeneration: 1n,
      ownerLeaseGeneration: 1n,
      storeVersion: 'target-v1'
    });
    let outcome;
    const join = stateful.joinActor(actor.ref, 'z-target', spot, spot.generation, undefined, 1000);
    const finished = join.promise.then(
      (value) => {
        outcome = value;
      },
      (error) => {
        outcome = { error };
      }
    );
    await f.until(() => outcome !== undefined, 'original Join terminal');
    await finished;
    assert.equal(outcome.terminalResult, RequestResult.Rejected);
    assert.equal(outcome.failureCode, 15);
    assert.equal(delivered, 1);
    assert.equal(f.targetRequests(), 1);
  } finally {
    stateful?.close();
    await f.close();
  }
});
