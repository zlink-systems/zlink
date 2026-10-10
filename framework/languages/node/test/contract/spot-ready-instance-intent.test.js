const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const generated = require('../../packages/framework/dist/runtime/protocol/service_wire_codec.generated');
const protocol = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { ZLinkRuntimeRouteTransport } = require('../../packages/framework/dist/runtime/channels/channel-transports');
const { SERVICE_WIRE_PREFIX_SIZE } = require('../../packages/framework/dist/runtime/foundation/service-wire-binary-primitives');
const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

const context = {};
const route = { targetNodeRid: 'owner-node', targetNodeGeneration: 2n, targetSpotId: 'room',
  objectGeneration: 3n, ownerId: 'owner', authorityOwnerGeneration: 4n,
  leaseGeneration: 5n, storeVersion: 'version' };

for (const instanceIntent of [false, true]) {
  test(`Ready route uses the generated Bool8 codec (${instanceIntent})`, () => {
    const header = wire.encodeInstanceSpotHeader(route, 1n, 'source', undefined,
      'request', { high: 1n, low: 2n }, 2n, false, instanceIntent);
    const expectedRoute = generated.encodeInstanceRouteV1({ routeKind: 'ready',
      targetNodeRid: Buffer.from(route.targetNodeRid), targetNodeGeneration: route.targetNodeGeneration,
      targetSpotId: route.targetSpotId, authority: { objectGeneration: route.objectGeneration,
        ownerId: route.ownerId, authorityOwnerGeneration: route.authorityOwnerGeneration,
        leaseGeneration: route.leaseGeneration, storeVersion: route.storeVersion },
      instanceIntent: instanceIntent ? 'true' : 'false' }, context);
    assert.deepEqual(header.subarray(SERVICE_WIRE_PREFIX_SIZE, SERVICE_WIRE_PREFIX_SIZE + expectedRoute.length),
      Buffer.from(expectedRoute));
    const decoded = wire.decodeStatefulHeader(header);
    assert.equal(decoded.activation, 'ready');
    assert.equal(decoded.instanceIntent, instanceIntent);
    assert.deepEqual(decoded.route, route);
    const malformed = Buffer.from(header);
    malformed[SERVICE_WIRE_PREFIX_SIZE + expectedRoute.length - 1] = 2;
    assert.throws(() => wire.decodeStatefulHeader(malformed), /bool8|enum/i);
    const payload = Buffer.from('{}');
    const frozen = Buffer.from(generated.encodeFrozenRecord({ recordKind: 'instanceSpotActivation',
      source: { sourceKind: 'node', sourceNodeRid: Buffer.from('source'), sourceNodeGeneration: 1n,
        sourceOwnerId: 'source-owner', sourceOwnerLeaseGeneration: 1n }, hasMetadata: 'false',
      operationId: { high: 1n, low: 2n }, operationKind: 'instanceSpotRequest',
      replyRoute: { originalOperationKind: 'instanceSpotRequest', replyRouteId: 2n },
      body: { recordKind: 'instanceSpotActivation', route: generated.decodeInstanceRouteV1(expectedRoute, context),
        sourceNodeGeneration: 1n, operationKind: 'request',
        payload: { packetName: 'Ping', contentType: 'application/json', payload } }
    }, { effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: payload.length,
      effectiveCompleteMessageBytes: encodeApplicationPayload({ packetName: 'Ping', contentType: 'application/json', payload }).length }));
    assert.deepEqual(wire.decodeServiceWireFrozenRecord(frozen).canonicalBytes, frozen);
    const routeOffset = frozen.indexOf(Buffer.from(expectedRoute));
    assert.notEqual(routeOffset, -1);
    frozen[routeOffset + expectedRoute.length - 1] = 2;
    assert.throws(() => wire.decodeServiceWireFrozenRecord(frozen), /bool8|enum/i);
  });

  test(`Ready send and request retain the source call intent (${instanceIntent})`, async () => {
    const frames = [];
    const owner = new ServiceStatefulRuntime({ setServiceIngress() {},
      async sendService(target, parts) { frames.push(parts[0]); return true; },
      async requestService(target, parts) {
        frames.push(parts[0]);
        const record = wire.decodeStatefulHeader(parts[0]);
        return [wire.encodeStatefulReply(record.replyRouteId, 0, 0)];
      } }, 'source', 1n);
    const payload = { packetName: 'Ping', contentType: 'application/json', payload: Buffer.from('{}') };
    await owner.sendToInstanceSpot(route, payload, undefined, undefined, instanceIntent);
    await owner.requestToInstanceSpot(route, payload, 1000, undefined, undefined, instanceIntent).promise;
    assert.deepEqual(frames.map((frame) => wire.decodeStatefulHeader(frame).instanceIntent),
      [instanceIntent, instanceIntent]);
    owner.close();
  });

  test(`Spot address Ready send and request pass the call intent to command 39 (${instanceIntent})`, async () => {
    class Ping {}
    const records = [];
    let replyParts;
    const target = { routerChannelId: 'mesh', targetNodeRid: route.targetNodeRid, spotId: route.targetSpotId,
      spotKind: framework.ZLinkSpotKind.Instance, stableType: 'Room', targetSpotGeneration: route.objectGeneration,
      targetNodeGeneration: route.targetNodeGeneration, authorityOwnerGeneration: route.authorityOwnerGeneration,
      targetOwnerId: route.ownerId, ownerLeaseGeneration: route.leaseGeneration, authorityStoreVersion: route.storeVersion };
    const node = {
      async sendToInstanceSpot(fence, payload, sourceSpot, metadata, intent) {
        records.push(wire.decodeStatefulHeader(wire.encodeInstanceSpotHeader(fence, 1n, 'source', undefined,
          'send', { high: 0n, low: 0n }, undefined, false, intent)));
        return 0;
      },
      requestInstanceSpot(fence, payload, timeout, sourceSpot, metadata, intent) {
        records.push(wire.decodeStatefulHeader(wire.encodeInstanceSpotHeader(fence, 1n, 'source', undefined,
          'request', { high: 1n, low: 2n }, 2n, false, intent)));
        replyParts = protocol.encodeChannelReplyParts(protocol.decodeChannelEnvelope(payload.map(ZLinkBufferMessage.from)).header, { ok: true }).map(ZLinkBufferMessage.from);
        return 2n;
      }
    };
    const completion = { async submit(submit) { submit(); return { terminalResult: 0, failureErrno: 0, parts: replyParts }; } };
    const routed = new ZLinkRuntimeRouteTransport(() => undefined, undefined,
      () => ({ meshNode: () => node, meshCompletionTable: () => completion }));
    const address = new internal.ZLinkHostSpotAddressTransport({ resolver: () => ({ resolve: async () => target }),
      routed, meshNames: () => ['mesh'], meshNode: () => node, completions: () => completion,
      defaultRequestTimeoutMs: 1000 });
    await address.sendToSpotAddress('room', new Ping(), { instanceSpot: instanceIntent });
    assert.deepEqual(await address.requestToSpotAddress('room', new Ping(), { instanceSpot: instanceIntent }), { ok: true });
    assert.deepEqual(records.map((record) => record.instanceIntent), [instanceIntent, instanceIntent]);
  });
}

test('Missing cold activation retains its original kind 2 contract', () => {
  const target = { targetNodeRid: 'owner-node', targetNodeGeneration: 2n, targetSpotId: 'room',
    stableType: 'Room', targetMeshName: 'mesh', descriptorVersion: '1' };
  const deadline = BigInt(Date.now() + 1000);
  const record = wire.decodeStatefulHeader(wire.encodeInstanceSpotActivationHeader(target, 1n, 'source', undefined,
    'request', { high: 1n, low: 2n }, deadline, 2n));
  assert.equal(record.activation, 'missing');
  assert.equal('instanceIntent' in record, false);
  assert.deepEqual(record.target, target);
  assert.equal(record.deadlineUnixMs, deadline);
});
