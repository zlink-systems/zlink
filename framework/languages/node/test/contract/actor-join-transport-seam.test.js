const assert = require('node:assert/strict');
const test = require('node:test');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { decodeActorJoin28 } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const { encodeStatefulReply } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

// The cross-language User-Spot Join stage proves the canonical Actor Join
// packet by decoding command 28 out of the frames the Node runtime hands to the
// service transport (framework/languages/node/cross-language/
// user_spot_join_host.js observes `requestService`). A remote Join must reach
// that one seam whichever sender owns it - direct or durable lifecycle - so the
// evidence never depends on which internal method the runtime picked.
const ACTOR_JOIN_WIRE_COMMAND = 28;

function joinFixture({ canonical, entry }) {
  const attempts = [];
  const runtime = new ServiceStatefulRuntime({
    observePeerConnectionIntentRemoved() { return () => {}; },
    setServiceIngress() {},
    topology: {
      peer: rid => rid === 'target-node'
        ? { descriptor: { lifecycleGeneration: 11n } }
        : undefined
    },
    async requestService(target, parts, timeoutMs) {
      const attempt = { target, parts, timeoutMs };
      attempts.push(attempt);
      const record = decodeActorJoin28(parts);
      return [
        encodeStatefulReply(record.correlation, RequestResult.Ok, 0, {
          kind: 'actorJoin',
          joinResult: 0,
          spot: { spotId: entry ? 'target-node' : 'target-spot', generation: entry ? 11n : 3n },
          membershipEpoch: 2n
        }),
        encodeApplicationPayload({
          packetName: 'JoinReply',
          contentType: 'application/json',
          payload: Buffer.from('{"joined":true}')
        })
      ];
    },
    // A remote Join that reached the local infrastructure lane or the plain
    // send lane instead of `requestService` fails the transport-seam assertion
    // below rather than silently producing no evidence.
    async reserveLocalIngress() { throw new Error('remote Join must not take the local ingress lane'); },
    async sendService() { throw new Error('remote Join must not take the fire-and-forget send lane'); }
  }, 'source-node', 7n);
  const actor = runtime.createActor('seam-actor');
  const spot = { spotId: entry ? 'target-node' : 'target-spot', generation: entry ? 11n : 3n };
  runtime.rememberSpotRoute({
    spot,
    targetNodeRid: 'target-node',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 13n,
    storeVersion: 'spot-version'
  });
  const payload = {
    packetName: canonical ? 'ZLinkFrameworkActorJoinRequest' : 'PrivateJoinRequest',
    contentType: 'application/json',
    payload: Buffer.from('{"seat":2}')
  };
  return {
    runtime,
    attempts,
    payload,
    join(timeoutMs) {
      const args = entry
        ? [actor.ref, 'target-node', payload]
        : [actor.ref, 'target-node', spot, spot.generation, payload];
      if (canonical) {
        args.push(
          { targetNodeGeneration: 7n, authorityOwnerGeneration: 1n, ownerLeaseGeneration: 7n },
          { phase: 'admission', transferId: 'local-only' }
        );
      }
      args.push(timeoutMs);
      const method = entry
        ? canonical ? 'joinActorEntrySpotCanonical' : 'joinActorEntrySpot'
        : canonical ? 'joinActorCanonical' : 'joinActor';
      return runtime[method](...args);
    }
  };
}

for (const canonical of [true, false]) {
  for (const entry of [true, false]) {
    test(`remote Actor Join hands command 28 to the service transport (canonical=${canonical}, entry=${entry})`, async () => {
      const fixture = joinFixture({ canonical, entry });
      try {
        const pending = fixture.join(5_000);
        const result = await pending.promise;
        assert.equal(result.terminalResult, RequestResult.Ok);
        assert.equal(fixture.attempts.length, 1);
        const [attempt] = fixture.attempts;
        assert.equal(attempt.target, 'target-node');
        assert.equal(
          attempt.parts[0][3],
          ACTOR_JOIN_WIRE_COMMAND,
          'the transport receives the command-28 Actor Join header frame'
        );
        const decoded = decodeActorJoin28(attempt.parts);
        assert.equal(decoded.correlation, pending.id);
        assert.equal(decoded.actor.id, 'seam-actor');
        assert.equal(decoded.entry, entry);
        assert.equal(
          decoded.payload.packetName,
          fixture.payload.packetName,
          'the application packet name stays readable at the transport seam'
        );
      } finally {
        fixture.runtime.close();
      }
    });
  }
}
