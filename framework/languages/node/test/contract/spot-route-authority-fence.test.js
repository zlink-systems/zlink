const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkRuntimeRouteTransport
} = require('../../packages/framework/dist/runtime/channels/channel-transports');
const {
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const {
  ServiceStaleGenerationError
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-registry');

function capturingTransport(capture, submissionError = new Error('captured')) {
  const node = {
    entrySpot() {
      return {
        requestToSpot(_nodeRid, _spotId, _generation, _parts, options) {
          capture(options);
          throw submissionError;
        }
      };
    }
  };
  return new ZLinkRuntimeRouteTransport(
    () => undefined,
    undefined,
    () => ({
      meshNode: () => node,
      meshCompletionTable: () => undefined
    })
  );
}

async function captureRawRequestOptions(target) {
  let captured;
  const transport = capturingTransport((options) => {
    captured = options;
  });
  const request = zlink.Message.from(Buffer.from(JSON.stringify({ packetName: 'Packet' })));
  try {
    await assert.rejects(
      transport.requestRawToSpot(target, request, {}),
      /captured/
    );
  } finally {
    request.close();
  }
  return captured;
}

test('direct Spot submission requires an authority fence only for authority-backed Spots', async () => {
  const entryOptions = await captureRawRequestOptions({
    routerChannelId: 'mesh',
    targetNodeRid: 'node-a',
    spotId: 'node-a',
    spotKind: framework.ZLinkSpotKind.Entry
  });
  assert.equal(entryOptions.routeFence, undefined);
  assert.equal(entryOptions.entrySpot, true);

  const userOptions = await captureRawRequestOptions({
    routerChannelId: 'mesh',
    targetNodeRid: 'node-b',
    spotId: 'room-1',
    spotKind: framework.ZLinkSpotKind.User,
    targetSpotGeneration: 7n,
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    targetOwnerId: 'owner-b',
    ownerLeaseGeneration: 17n,
    authorityStoreVersion: 'store-19'
  });
  assert.equal(userOptions.entrySpot, false);
  assert.deepEqual(userOptions.routeFence, {
    spot: { spotId: 'room-1', generation: 7n },
    targetNodeRid: 'node-b',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n,
    storeVersion: 'store-19'
  });
});

test('raw stale authority submission becomes a retryable Spot generation error', async () => {
  const transport = capturingTransport(
    () => {},
    new ServiceStaleGenerationError('spot', 'room-1')
  );
  const request = zlink.Message.from(Buffer.from(JSON.stringify({ packetName: 'Packet' })));
  try {
    await assert.rejects(
      transport.requestRawToSpot({
        routerChannelId: 'mesh',
        targetNodeRid: 'node-b',
        spotId: 'room-1',
        spotKind: framework.ZLinkSpotKind.User,
        targetSpotGeneration: 7n,
        targetNodeGeneration: 11n,
        authorityOwnerGeneration: 13n,
        targetOwnerId: 'owner-b',
        ownerLeaseGeneration: 17n,
        authorityStoreVersion: 'store-19'
      }, request, {}),
      (error) => {
        assert.equal(
          internalFrameworkErrorKind(error),
          ZLinkFrameworkInternalErrorKind.SpotGenerationStale
        );
        assert.match(error.message, /stale Spot authority fence/);
        return true;
      }
    );
  } finally {
    request.close();
  }
});
