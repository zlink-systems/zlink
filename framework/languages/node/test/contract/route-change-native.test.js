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

test('cross-node Join retains its original terminal across manual plus automatic route replacement', async () => {
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
      'original request was discarded before the source observed replacement'
    );
    await f.until(() => outcome !== undefined, 'original Join terminal');
    assert.equal(outcome.terminalResult, RequestResult.Rejected);
    assert.equal(outcome.failureCode, 15);
    assert.equal(delivered, 1);
    await finished;
  } finally {
    stateful?.close();
    await f.close();
  }
});
