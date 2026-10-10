const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  encodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

test('cold event and request have distinct operation identities on the same source', async () => {
  const records = [];
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      async sendService(target, parts) {
        records.push(wire.decodeStatefulHeader(parts[0]));
        return true;
      },
      async requestService(target, parts) {
        const record = wire.decodeStatefulHeader(parts[0]);
        records.push(record);
        return [wire.encodeStatefulReply(record.replyRouteId, 0, 0)];
      }
    },
    'source',
    1n
  );
  const target = {
    targetSpotId: 'room',
    stableType: 'Room',
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    targetMeshName: 'mesh',
    descriptorVersion: '1'
  };
  const payload = encodeApplicationPayload({
    packetName: 'Query',
    contentType: 'application/json',
    payload: Buffer.from('{}')
  });
  try {
    await runtime.sendToMissingInstanceSpotFrame(target, payload, BigInt(Date.now() + 5000));
    const pending = runtime.requestToMissingInstanceSpotFrame(
      target,
      payload,
      BigInt(Date.now() + 5000)
    );
    await pending.promise;
    assert.equal(records.length, 2);
    assert.notDeepEqual(records[0].operation, records[1].operation);
  } finally {
    runtime.close();
  }
});

test('same Spot ID shares activation before subsequent operations enter the application queue', async () => {
  const runtime = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'target', 7n);
  let release;
  const ready = new Promise((resolve) => {
    release = resolve;
  });
  let activations = 0;
  const queued = [];
  const failed = [];
  runtime.validateInstanceIngress = () => {};
  runtime.runMissingInstanceActivation = async () => {
    ++activations;
    await ready;
    return {
      spot: { ref: { spotId: 'room', generation: 1n } },
      route: {
        targetSpotId: 'room',
        targetNodeRid: 'target',
        targetNodeGeneration: 7n,
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n,
        ownerId: 'owner',
        leaseGeneration: 1n,
        storeVersion: 'v1'
      }
    };
  };
  runtime.activationTerminalCompletion = () => ({});
  runtime.enqueueActivatedInstanceSpot = (ingress, record) => {
    queued.push(record.operation.low);
    return 'application';
  };
  runtime.finishMissingInstanceActivationFailure = (ingress, record, error) => {
    failed.push({ operation: record.operation.low, error });
  };
  const record = (operation, type = 'Room') => ({
    kind: 'instanceSpot',
    activation: 'missing',
    target: {
      targetSpotId: 'room',
      stableType: type,
      targetNodeRid: 'target',
      targetNodeGeneration: 7n,
      targetMeshName: 'mesh',
      descriptorVersion: '1'
    },
    operation: { high: 1571n, low: operation },
    sourceNodeGeneration: 1n,
    operationKind: operation === 1n ? 'send' : 'request',
    deadlineUnixMs: BigInt(Date.now() + 5000)
  });
  try {
    const event = runtime.continueMissingInstanceActivation({}, record(1n), Buffer.from('{}'));
    const request = runtime.continueMissingInstanceActivation({}, record(2n), Buffer.from('{}'));
    const differentType = runtime.continueMissingInstanceActivation(
      {},
      record(3n, 'Other'),
      Buffer.from('{}')
    );
    assert.equal(activations, 1);
    release();
    await Promise.all([event, request, differentType]);
    assert.deepEqual(queued, [1n, 2n]);
    assert.equal(failed.length, 1);
    assert.equal(failed[0].operation, 3n);
  } finally {
    release();
    runtime.close();
  }
});
