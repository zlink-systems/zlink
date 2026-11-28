const assert = require('node:assert/strict');
const test = require('node:test');
const { replyFixture } = require('./helpers/relocation-reply');
const { relocationAuthorityStore } = require('./fixtures/relocation-authority-store');
const {
  ReceiveKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  ZLinkActorHandoffCoordinator,
  ZLinkActorTransferRuntime,
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const codec = require('../../packages/framework/dist/runtime/protocol/service_wire_codec.generated');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  encodeChannelEnvelopeParts,
  decodeChannelReply
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  encodeApplicationPayload,
  encodeMultipartPayload,
  decodeMultipartParts
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  encodeServiceRelocationEnvelope,
  decodeServiceRelocationEnvelope,
  crc32c
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

const context = {
  effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 1024,
  effectiveCompleteMessageBytes: 4096
};
const key = encodeAuthorityKey('instance_spot', 'room');
const source = {
  nodeRid: 'source',
  nodeGeneration: 7n,
  ownerId: 'source-owner',
  leaseGeneration: 11n
};
const route = {
  targetNodeRid: 'target',
  targetNodeGeneration: 13n,
  targetSpotId: 'room',
  objectGeneration: 17n,
  ownerId: 'target-owner',
  authorityOwnerGeneration: 19n,
  leaseGeneration: 23n,
  storeVersion: 'store-version'
};
const requestOperation = { high: 1n << 63n, low: 37n };
const sendOperation = { high: 0n, low: 0n };
const payload = {
  packetName: 'room-packet',
  contentType: 'application/octet-stream',
  payload: Buffer.from([3, 5, 8])
};
const metadata = Buffer.from([1, 1, 1, 107, 0, 1, 118]);

function frozenRecord(
  request,
  { application = payload, operation = requestOperation, replyRouteId = 41n } = {}
) {
  const operationKind = request ? 'instanceSpotRequest' : 'none';
  return Buffer.from(
    codec.encodeFrozenRecord(
      {
        recordKind: 'instanceSpotActivation',
        source: {
          sourceKind: 'spot',
          sourceNodeRid: Buffer.from(source.nodeRid),
          sourceNodeGeneration: source.nodeGeneration,
          sourceOwnerId: source.ownerId,
          sourceOwnerLeaseGeneration: source.leaseGeneration,
          sourceSpotId: 'source-room'
        },
        hasMetadata: 'true',
        metadata: codec.decodeMetadataFrame(metadata, context),
        operationId: request ? operation : sendOperation,
        operationKind,
        replyRoute: request
          ? { originalOperationKind: operationKind, replyRouteId }
          : { originalOperationKind: operationKind },
        body: {
          recordKind: 'instanceSpotActivation',
          route: {
            routeKind: 'ready',
            targetNodeRid: Buffer.from(route.targetNodeRid),
            targetNodeGeneration: route.targetNodeGeneration,
            targetSpotId: route.targetSpotId,
            instanceIntent: 'true',
            authority: {
              objectGeneration: route.objectGeneration,
              ownerId: route.ownerId,
              authorityOwnerGeneration: route.authorityOwnerGeneration,
              leaseGeneration: route.leaseGeneration,
              storeVersion: route.storeVersion
            }
          },
          sourceNodeGeneration: source.nodeGeneration,
          operationKind: request ? 'request' : 'send',
          payload: application
        }
      },
      context
    )
  );
}

function assertFrozen(bytes, request) {
  const record = codec.decodeFrozenRecord(bytes, context);
  assert.equal(record.recordKind, 'instanceSpotActivation');
  assert.equal(record.operationKind, request ? 'instanceSpotRequest' : 'none');
  assert.deepEqual(record.operationId, request ? requestOperation : sendOperation);
  assert.equal(record.replyRoute.replyRouteId, request ? 41n : undefined);
  assert.deepEqual(Buffer.from(record.body.payload.payload), payload.payload);
}

// Emitted by the Java generated codec at 913d848ff4 using the inputs from
// ZLinkInstanceSpotFrozenRecordTest.encodeAccepted.
const javaFrozenRecords = [
  '0e02003006736f7572636500000000000000070c736f757263652d6f776e6572000000000000000b0b736f757263652d726f' +
    '6f6d010101016b0001760000000000000000000000000000000000000000000001004906746172676574000000000000000d' +
    '04726f6f6d00000000000000110c7461726765742d6f776e657200000000000000130000000000000017000d73746f72652d' +
    '76657273696f6e01000000000000000701010000002c0b726f6f6d2d7061636b6574186170706c69636174696f6e2f6f6374' +
    '65742d73747265616d00000003030508',
  '0e02003006736f7572636500000000000000070c736f757263652d6f776e6572000000000000000b0b736f757263652d726f' +
    '6f6d010101016b000176800000000000000000000000000000250000000c0008000000000000002901004906746172676574' +
    '000000000000000d04726f6f6d00000000000000110c7461726765742d6f776e657200000000000000130000000000000017' +
    '000d73746f72652d76657273696f6e01000000000000000702010000002c0b726f6f6d2d7061636b6574186170706c696361' +
    '74696f6e2f6f637465742d73747265616d00000003030508'
].map((hex) => Buffer.from(hex, 'hex'));

test('reads Java Instance Spot frozen send and request records', () => {
  for (const [index, bytes] of javaFrozenRecords.entries()) {
    const request = index === 1;
    assert.deepEqual(bytes, frozenRecord(request));
    assertFrozen(bytes, request);
    const decoded = wire.decodeServiceWireFrozenRecord(bytes);
    assert.equal(decoded.sourceSpotId, 'source-room');
    assert.equal(decoded.source.nodeGeneration, 7n);
    assert.equal(decoded.instanceSpot?.route.targetSpotId, 'room');
    assert.equal(decoded.instanceSpot?.route.objectGeneration, 17n);
    assert.equal(decoded.instanceSpot?.instanceIntent, true);
  }
});

const relocation = { high: 1n, low: 2n };
const relocationId = '00000000-0000-0001-0000-000000000002';
const object = { kind: 'instanceSpot', stableType: 'Room', spotId: 'room', objectGeneration: 17n };
const coordinator = {
  ownerId: route.ownerId,
  leaseGeneration: route.leaseGeneration,
  nodeRid: route.targetNodeRid,
  nodeGeneration: route.targetNodeGeneration,
  expectedAuthorityStoreVersion: route.storeVersion
};
const target = {
  nodeRid: 'next-target',
  nodeGeneration: 29n,
  ownerId: 'next-owner',
  ownerLeaseGeneration: 31n
};

function application(label, request) {
  return {
    packetName: SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
    contentType: SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
    payload: encodeMultipartPayload(
      encodeChannelEnvelopeParts(
        request ? 1 : 3,
        'mesh',
        'RoomPacket',
        { label },
        request ? 10_000 : undefined
      ).map((part) => ZLinkBufferMessage.from(part).data())
    )
  };
}

async function targetFixture(queuedMessages, seen, sendReply) {
  const jobs = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 1n));
  let permitsAcquired = 0;
  class Room {}
  class RoomPacket {
    handle(spot, message) {
      assert.equal(permitsAcquired, seen.length + 1);
      seen.push([spot, message.label]);
      return { echoed: message.label };
    }
  }
  const spotManager = new DefaultZLinkSpotManager({
    detachedTaskRunner: {},
    spotFactories: [],
    spotPacketHandlers: [{ spotType: Room, handlerType: RoomPacket, packetName: 'RoomPacket' }]
  });
  const writer = { ownerId: target.ownerId, leaseGeneration: target.ownerLeaseGeneration };
  const replies = replyFixture(writer);
  await replies.setWriter(writer);
  const authorityFence = {
    ownerId: writer.ownerId,
    ownerLeaseGeneration: writer.leaseGeneration,
    nodeRid: Buffer.from(target.nodeRid).toString('hex'),
    nodeGeneration: target.nodeGeneration
  };
  const locationStore = relocationAuthorityStore(async () => ({
    kind: 'snapshot',
    storeVersion: { value: '1' },
    payload: Buffer.from('application'),
    objectGeneration: 17n,
    authorityOwnerGeneration: 53n,
    ownerId: writer.ownerId,
    ownerLeaseGeneration: writer.leaseGeneration,
    relocation: {
      relocationId,
      phase: 'committed',
      source: authorityFence,
      target: { ...authorityFence, attemptGeneration: 1n },
      coordinator: { ...authorityFence, expectedStoreVersion: 'source-version' },
      sourceInventorySha256: 'ab'.repeat(32),
      applicationVersion: 1n
    },
    allocation: {
      objectKind: 'instance_spot',
      stableType: 'Room',
      descriptor: { rid: target.nodeRid },
      descriptorLifecycleGeneration: target.nodeGeneration
    }
  }))();
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([
        [
          'mesh',
          {
            instanceSpotFactoryRegistrations: {
              Room: { implementation: Room, relocation: { kind: 'recreate' } }
            }
          }
        ]
      ])
    },
    spotManager: () => spotManager,
    acquireSpotReplayPermit(signal) {
      permitsAcquired += 1;
      return jobs.acquire(signal, 'remote');
    },
    currentOwner: () => ({ ownerId: target.ownerId, leaseGeneration: target.ownerLeaseGeneration }),
    locationStore: () => ({
      ...locationStore,
      relocationReplies: replies.store,
      async readOwnerLease(ownerId) {
        assert.equal(ownerId, source.ownerId);
        return {
          kind: 'held',
          token: source,
          leaseExpiresAt: new Date(200),
          storeNow: new Date(100)
        };
      },
      async commitAggregate(fence) {
        assert.equal(fence.capturedRequests.length, 1);
        assert.equal(fence.capturedRequests[0].objectKey, key.value);
        assert.deepEqual(seen, []);
        await replies.seed(fence.capturedRequests[0]);
        return { kind: 'committed' };
      }
    }),
    liveDescriptors: async () => [
      peerDescriptor(source.nodeRid, source.nodeGeneration, source.ownerId, source.leaseGeneration)
    ],
    meshNode: () => ({
      status: () => ({ routingId: target.nodeRid, lifecycleGeneration: target.nodeGeneration }),
      sendToNode: sendReply
    })
  });
  runtime.readAggregateForCommitRetry = async () => ({
    kind: 'committed',
    authorities: new Map([[key.value, await locationStore.readAuthority(key)]])
  });
  const materialized = await runtime.materializeTargetEnvelope('mesh', {
    aggregateId: relocationId,
    aggregateGeneration: 1n,
    participants: [
      {
        participantId: 1n,
        key: key.value,
        objectKind: 'instance_spot',
        stableType: 'Room',
        objectGeneration: 17n,
        authorityOwnerGeneration: 19n,
        applicationState: Buffer.alloc(0),
        boundSessionState: Buffer.alloc(0),
        queuedMessages,
        timers: []
      }
    ],
    memberships: []
  });
  return {
    runtime,
    replies,
    spotManager,
    ...materialized,
    offer: {
      prepare: {
        target,
        object,
        coordinator,
        relocation,
        targetAttemptGeneration: target.nodeGeneration
      },
      reservation: { prepared: { fence: {}, plan: {} } },
      authenticatedSourceNodeRid: coordinator.nodeRid,
      prepareFingerprint: relocationId
    },
    phase: 'ready',
    lane: Promise.resolve(),
    boundaryRelay: [],
    releaseActivation() {},
    releaseJoinAdmission() {}
  };
}

function peerDescriptor(rid, lifecycleGeneration, ownerId, leaseGeneration) {
  return { rid, lifecycleGeneration, ownerId, leaseGeneration, state: 1 };
}

function receiveControl(runtime, sender, frames) {
  return runtime.tryHandleControl('mesh', {
    kind: ReceiveKind.NodeSend,
    sourceNodeRid: sender.nodeRid,
    sourceNodeGeneration: sender.nodeGeneration,
    parts: (Array.isArray(frames) ? frames : [frames]).map(ZLinkBufferMessage.from)
  });
}

for (const relay of [false, true]) {
  test(
    relay
      ? 'cutover replays command 31 Instance Spot send and request after saved work'
      : 'saved work replays Instance Spot send and request on the restored handler',
    async () => {
      const seen = [];
      let originalRequest;
      const requester = new ServiceStatefulRuntime(
        {
          setServiceIngress() {},
          isPeerRouteReady: () => true,
          requestService(_rid, parts) {
            originalRequest = wire.decodeStatefulHeader(parts[0]);
            return new Promise(() => {});
          }
        },
        source.nodeRid,
        source.nodeGeneration
      );
      const pending = requester.requestToInstanceSpot(route, application('request', true), 10_000);
      const reply = pending.promise;
      void reply.catch(() => {});
      const savedSend = frozenRecord(false, { application: application('saved-send', false) });
      const request = frozenRecord(true, {
        application: application('request', true),
        operation: originalRequest.operation,
        replyRouteId: originalRequest.replyRouteId
      });
      const saved = (relay ? [savedSend] : [savedSend, request]).map((payload, index) => ({
        sequence: BigInt(index + 1),
        payload
      }));
      const acknowledgements = [];
      const caller = new ZLinkHostServiceRelocationRuntime({
        actorTransfer: new ZLinkActorTransferRuntime({
          actorHandoff: new ZLinkActorHandoffCoordinator({})
        }),
        currentOwner: () => source,
        liveDescriptors: async () => [
          peerDescriptor(
            target.nodeRid,
            target.nodeGeneration,
            target.ownerId,
            target.ownerLeaseGeneration
          )
        ],
        meshNode: () => ({
          status: () => ({ routingId: source.nodeRid, lifecycleGeneration: source.nodeGeneration }),
          acceptRelocatedActorTerminal: requester.acceptRelocatedActorTerminal.bind(requester),
          async sendToNode(rid, frame) {
            assert.equal(rid, target.nodeRid);
            const ack = wire.decodeMaintenanceReplyRelayAck(frame);
            assert.deepEqual(ack.requestSource, source);
            assert.deepEqual(ack.operation, originalRequest.operation);
            assert.equal(ack.replyRouteId, originalRequest.replyRouteId);
            acknowledgements.push(ack.status);
            await receiveControl(stage.runtime, source, frame);
            return SubmitResult.Ok;
          }
        })
      });
      let replyFrames;
      const stage = await targetFixture(saved, seen, async (rid, frames) => {
        assert.equal(rid, source.nodeRid);
        replyFrames = frames;
        await receiveControl(caller, target, frames);
        return SubmitResult.Ok;
      });
      const stagingId = '1:2:29';
      stage.runtime.targetStages.set(stagingId, stage);
      try {
        const encoded = encodeServiceRelocationEnvelope(stage.staging.envelope, 0n);
        const decoded = decodeServiceRelocationEnvelope(encoded, 1n, 'instanceSpot');
        assert.deepEqual(decoded.participants[0].queuedMessages, saved);
        await stage.owner.restoreSavedWork(stage.staging);
        assert.deepEqual(seen, []);

        const control = {
          relocation,
          targetAttemptGeneration: target.nodeGeneration,
          coordinator,
          senderRole: 'source',
          object
        };
        const records = relay
          ? [frozenRecord(false, { application: application('relay-send', false) }), request]
          : [];
        for (const bytes of records) {
          const command = wire.encodeMaintenanceRelocationControl({
            ...control,
            kind: 'data',
            frozenRecord: wire.decodeServiceWireFrozenRecord(bytes)
          });
          assert.equal(await receiveControl(stage.runtime, coordinator, command), true);
        }
        const cutover = {
          ...control,
          kind: 'cutover',
          boundaryRecordCount: BigInt(records.length),
          boundaryChecksumCrc32c: crc32c(Buffer.concat(records))
        };
        if (relay) {
          assert.throws(
            () =>
              stage.runtime.reconcileBoundaryRelay(
                stage,
                {
                  ...cutover,
                  boundaryChecksumCrc32c: 0
                },
                stagingId
              ),
            /boundary confirmation mismatch/
          );
        }
        assert.deepEqual(seen, [], 'staged commands must wait for target commit');
        assert.equal(
          await receiveControl(
            stage.runtime,
            coordinator,
            wire.encodeMaintenanceRelocationControl(cutover)
          ),
          true
        );
        await stage.finalize;
        await stage.staging.hidden.get(key.value).replayCompletion;
        assert.deepEqual(
          (await stage.replies.rows(relocationId)).map(({ state }) => state),
          ['terminalReceived']
        );
        await receiveControl(caller, target, replyFrames);
        const result = await reply;
        assert.deepEqual(
          decodeChannelReply(
            decodeMultipartParts(result.payload.payload, ZLinkBufferMessage.fromOwned)
          ),
          { echoed: 'request' }
        );
        const activation = stage.spotManager.resolveRelocationActivation('mesh', 'room');
        assert.deepEqual(
          seen,
          (relay ? ['saved-send', 'relay-send', 'request'] : ['saved-send', 'request']).map(
            (label) => [activation.spot, label]
          )
        );
        assert.deepEqual(acknowledgements, ['terminalReceived', 'alreadyTerminal']);
      } finally {
        requester.close();
        await stage.owner.abort(stage.staging);
      }
    }
  );
}

test('source holds Ready Instance Spot ingress and freezes the original send and request IDs', async () => {
  const admitted = [];
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      mailbox: {
        tryEnqueue(record) {
          admitted.push(record);
          record.applicationJob.close();
          return true;
        }
      }
    },
    'target',
    13n
  );
  try {
    runtime.restoreSpotAuthority('room', 'instance_spot', 'Room', 17n, 19n);
    const seal = runtime.sealSpotMessageFollowIngress({
      spot: { spotId: 'room', generation: 17n },
      targetNodeRid: 'target',
      targetNodeGeneration: 13n,
      authorityOwnerGeneration: 19n,
      ownerLeaseGeneration: 23n,
      storeVersion: 'store-version'
    });
    const sources = [];
    runtime.setRequestSourceAcceptance((rid, generation) => {
      sources.push([rid, generation]);
      return source;
    });
    const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 8n));
    const records = [];
    for (const request of [false, true]) {
      const header = wire.encodeInstanceSpotHeader(
        route,
        7n,
        'source',
        'source-room',
        request ? 'request' : 'send',
        request ? requestOperation : sendOperation,
        request ? 41n : undefined,
        true,
        true
      );
      const parts = [header, metadata, encodeApplicationPayload(payload)];
      const applicationJobOwner = ApplicationIngressRecordOwner.create(
        queue,
        await queue.acquire(undefined, 'remote'),
        { close() {} }
      );
      try {
        await runtime.handleIngress(
          {
            sourceRoutingId: 'source',
            sourceNodeGeneration: 7n,
            parts,
            flags: wire.M6bServiceWireFlag.metadata,
            applicationJobOwner,
            requestSequence: request ? 41n : undefined
          },
          wire.decodeStatefulHeader(header),
          parts[2],
          metadata
        );
      } finally {
        applicationJobOwner.close();
      }
      assert.deepEqual(admitted, []);
      const batch = await runtime.freezeSpotRelocationIngress(seal);
      assert.equal(batch.length, 1);
      records.push(batch[0]);
    }
    assert.deepEqual(admitted, []);
    assert.deepEqual(await runtime.freezeSpotRelocationIngress(seal), []);
    assert.deepEqual(sources, [
      [source.nodeRid, source.nodeGeneration],
      [source.nodeRid, source.nodeGeneration]
    ]);
    records.forEach((record, index) => {
      assertFrozen(record.canonicalBytes, index === 1);
      assert.deepEqual(record.canonicalBytes, javaFrozenRecords[index]);
    });
    assert.equal(runtime.abortSpotMessageFollowIngress(seal), true);
    assert.deepEqual(
      admitted.map(({ stateful }) => stateful.activationRecord.operation),
      [sendOperation, requestOperation]
    );
  } finally {
    runtime.close();
  }
});
