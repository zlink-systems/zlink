'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { SubmitResult } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const deferred = require('./helpers/deferred.cjs');
const telemetry = require('./helpers/telemetry-log-capture');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  M6bServiceWireCommand,
  encodeInstanceSpotHeader
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  encodeApplicationPayload,
  encodeMultipartPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  encodeServiceMetadataFrame
} = require('../../packages/framework/dist/runtime/foundation/service-metadata-codec');
const {
  decodeInstanceSpotFollowCommand,
  encodeInstanceSpotFollowCommand
} = require('../../packages/framework/dist/runtime/protocol/service_wire_codec.generated');
const {
  ReadyDomain,
  ReceiveKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

const {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  ServiceMailbox
} = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');

const context = {
  effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 0xffff_fdf6,
  effectiveCompleteMessageBytes: 0xffff_ffff
};
const original = { rid: 'caller', generation: 3n };
const source = {
  targetSpotId: 'room',
  targetNodeRid: 'previous',
  targetNodeGeneration: 5n,
  objectGeneration: 7n,
  authorityOwnerGeneration: 11n,
  leaseGeneration: 13n,
  ownerId: 'previous-owner',
  storeVersion: 'previous-version'
};
const target = {
  ...source,
  targetNodeRid: 'target',
  targetNodeGeneration: 17n,
  authorityOwnerGeneration: 12n,
  leaseGeneration: 19n,
  ownerId: 'target-owner',
  storeVersion: 'target-version'
};
const metadata = encodeServiceMetadataFrame(new Map([['trace', 'follow']]));
const application = {
  packetName: SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
  contentType: SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  payload: encodeMultipartPayload(
    encodeChannelEnvelopeParts(3, 'instance', 'Notice', { test: true })
  )
};
const payload = encodeApplicationPayload(application);

function directFence(route) {
  return {
    spot: { spotId: route.targetSpotId, generation: route.objectGeneration },
    targetNodeRid: route.targetNodeRid,
    targetNodeGeneration: route.targetNodeGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration,
    ownerLeaseGeneration: route.leaseGeneration,
    storeVersion: route.storeVersion
  };
}

function readyParts(route = source) {
  return [
    encodeInstanceSpotHeader(
      route,
      original.generation,
      original.rid,
      'caller-spot',
      'send',
      { high: 0n, low: 0n },
      undefined,
      true
    ),
    metadata,
    payload
  ];
}

function followParts(route, hop, followApplication = application) {
  return encodeInstanceSpotFollowCommand(
    {
      command: 'instanceSpotFollow',
      flags: 0,
      operationHigh: 0n,
      operationLow: 0n,
      sourceNodeGeneration: original.generation,
      sourceNodeRid: Buffer.from(original.rid),
      sourceSpotId: null,
      messageFollowHopCount: hop,
      targetSpot: {
        spot: { spotId: route.targetSpotId, objectGeneration: route.objectGeneration },
        targetNodeRid: Buffer.from(route.targetNodeRid),
        targetNodeGeneration: route.targetNodeGeneration,
        expectedAuthorityOwnerGeneration: route.authorityOwnerGeneration,
        expectedOwnerLeaseGeneration: route.leaseGeneration
      },
      payload: followApplication
    },
    context
  ).map((part) => Buffer.from(part));
}

function fixture(t, route) {
  const sent = [];
  const mailbox = new ServiceMailbox();
  const diagnostics = [];
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 1n));
  let ingress;
  let accepts;
  const raw = {
    mailbox,
    setServiceIngress(handler, predicate) {
      ingress = handler;
      accepts = predicate;
    },
    sendApplication(rid, parts, generation) {
      sent.push({ rid, generation, parts: parts.map((part) => Buffer.from(part)) });
      return true;
    },
    sendService(rid, parts) {
      sent.push({ rid, parts });
      return true;
    }
  };
  const runtime = new ServiceStatefulRuntime(raw, route.targetNodeRid, route.targetNodeGeneration);
  const backend = new ZLinkNodeRawMeshBackend(
    'mesh',
    route.targetNodeRid,
    {},
    queue,
    undefined,
    undefined,
    undefined,
    { raw, stateful: runtime }
  );
  runtime.restoreSpotAuthority(
    route.targetSpotId,
    'instance_spot',
    'room',
    route.objectGeneration,
    route.authorityOwnerGeneration
  );
  runtime.registerInstanceIntent('room', route);
  runtime.setDispatchErrorReporter(
    { captureEnabled: () => true, report: (diagnostic) => diagnostics.push(diagnostic) },
    'mesh'
  );
  t.after(() => {
    mailbox.close();
    runtime.close();
  });
  return {
    sent,
    backend,
    pending: () => mailbox.pendingMessages('application'),
    diagnostics,
    async receive(parts, peer) {
      const command = parts[0][3];
      if (!accepts(command)) return 'protocolError';
      const owner = ApplicationIngressRecordOwner.create(
        queue,
        await queue.acquire(undefined, 'remote'),
        { close() {} }
      );
      try {
        return await ingress({
          command,
          flags: parts[0][4],
          sourceRoutingId: peer.rid,
          sourceNodeGeneration: peer.generation,
          parts,
          applicationJobOwner: owner
        });
      } finally {
        owner.close();
      }
    },
    async relocate(next, duration = 30_000) {
      const fence = directFence(route);
      const seal = runtime.sealSpotMessageFollowIngress(fence);
      assert.ok(seal);
      assert.equal(
        await runtime.commitSpotMessageFollowIngress(seal, next && directFence(next), duration),
        true
      );
      assert.equal(runtime.registry.closeSpot(fence.spot), true);
    }
  };
}

const previousPeer = { rid: source.targetNodeRid, generation: source.targetNodeGeneration };

test('Ready Instance one-way reaches the new owner with its original sender and no completion', async (t) => {
  const previous = fixture(t, source);
  const current = fixture(t, target);
  await previous.relocate(target);

  await previous.receive(readyParts(), original);
  assert.equal(previous.sent.length, 1, 'previous owner must relay the Ready one-way');
  assert.equal(previous.sent[0].rid, target.targetNodeRid);
  assert.equal(previous.sent[0].generation, target.targetNodeGeneration);
  const { parts } = previous.sent[0];
  assert.equal(parts[0][3], M6bServiceWireCommand.instanceSpotFollow);
  const follow = decodeInstanceSpotFollowCommand(parts, context);
  assert.equal(follow.operationHigh, 0n);
  assert.equal(follow.operationLow, 0n);
  assert.equal(follow.sourceNodeGeneration, original.generation);
  assert.equal(follow.messageFollowHopCount, 1);
  assert.deepEqual(parts.slice(1), [metadata, payload]);
  assert.equal(await current.receive(parts, previousPeer), 'application');
  assert.equal(previous.pending(), 0);
  assert.equal(current.pending(), 1);
  const handled = [];
  class Room {}
  class Notice {
    handle(_room, message) {
      handled.push(message);
    }
  }
  const manager = new DefaultZLinkSpotManager({
    detachedTaskRunner: new ZLinkRuntimeTaskRunner(
      new ZLinkRuntimeTaskErrorSink(),
      new AbortController().signal
    ),
    spotFactories: [],
    instanceSpotFactories: new Map([['mesh', new Map([['room', Room]])]]),
    spotPacketHandlers: [{ spotType: Room, handlerType: Notice, packetName: 'Notice' }],
    instanceSpotApplicationTargetProvider: () => ({
      stableType: 'room',
      objectGeneration: target.objectGeneration
    })
  });
  const ready = current.backend.createReadyBatch(1);
  const batch = current.backend.createReceiveBatch(1, 2);
  const [owner] = current.backend.drainReady(ReadyDomain.Application, ready).records;
  const claim = ready.takeClaim(0);
  const [record] = claim.recvBatch(batch).records;
  try {
    assert.equal(record.kind, ReceiveKind.InstanceSpotActivation);
    assert.equal(record.sourceNodeRid, original.rid);
    assert.equal(record.activationRecord.sourceNodeGeneration, original.generation);
    assert.equal(record.sourceSpotId, 'caller-spot');
    assert.deepEqual(record.applicationMetadata, metadata);
    assert.deepEqual(record.operationId, { high: 0n, low: 0n });
    await manager.dispatchMeshInstance('mesh', owner, record);
    assert.deepEqual(handled, [{ test: true }]);
  } finally {
    for (const part of record.parts) part.close();
    record.releaseRetainedIngress();
    claim.release();
    batch.close();
    ready.close();
    await manager.close('mesh', target.targetSpotId);
  }
  assert.equal(previous.diagnostics.length + current.diagnostics.length, 0);
});

test(
  'committed Instance Spot admits Follow before target publication',
  { timeout: 30_000 },
  async (t) => {
    const hostClock = fixedRidHostClock(t);
    const mesh = 'instance-follow-barrier';
    const spotId = 'room';
    const callerRid = 'follow-caller';
    const key = encodeAuthorityKey('instance_spot', spotId);
    const locationStore = new ZLinkInMemoryProviderLocationStore();
    const relocationStore = new MemoryRelocationStore();
    const committed = deferred();
    const publish = deferred();
    const created = deferred();
    const delivered = deferred();
    const handled = { previous: [], target: [] };
    const apps = [];
    let moving;
    t.after(async () => {
      publish.resolve();
      try {
        await moving;
      } finally {
        for (const app of apps.reverse()) await app.close();
      }
    });

    async function start(rid, peer) {
      const builder = nest
        .zlinkFramework()
        .addLocationStore(locationStore)
        .addRelocationStore(relocationStore);
      builder.configureDispatch().messageFlow('normal');
      builder.configureLocations().pollingIntervalMs(20).ownerLeaseFencingMarginMs(1);
      const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(rid);
      route.channel(mesh).server();
      if (peer !== undefined) route.peerConnections().connect(peer.rid, peer.descriptor.endpoint);
      class App {}
      class Room {}
      class NoticeHandler {
        handle(_spot, notice) {
          if (notice.marker === 'create') {
            created.resolve();
            return;
          }
          handled[rid].push(notice);
          if (rid === 'target') delivered.resolve();
        }
      }
      if (rid === callerRid) route.objects().client();
      else {
        Injectable()(Room);
        Injectable()(NoticeHandler);
        nest.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'Notice' })(NoticeHandler);
        route
          .objects()
          .server()
          .addInstanceSpotFactory('room', Room, (factory) => factory.recreateOnRelocation());
      }
      Module({
        imports: [nest.ZLinkModule.forRoot(builder.build())],
        providers: rid === callerRid ? [] : [Room, NoticeHandler]
      })(App);
      const app = await NestFactory.createApplicationContext(App, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      await finishFixedRidHostStartup(
        host,
        hostClock,
        framework.ZLinkFrameworkRuntimeState.Preparing
      );
      const node = host.spotNodeRuntime.meshNode(mesh);
      const query = app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
      while (!t.signal.aborted) {
        const descriptor = (await query.listMeshNodeDescriptors(mesh)).items.find(
          (item) => String(item.rid) === rid && item.state === 1
        );
        if (descriptor !== undefined) return { app, host, node, rid, descriptor };
        await delay(10, undefined, { signal: t.signal });
      }
      t.signal.throwIfAborted();
    }

    function snapshotRoute(snapshot) {
      assert.equal(snapshot.kind, 'snapshot');
      return {
        targetSpotId: spotId,
        targetNodeRid: String(snapshot.allocation.descriptor.rid),
        targetNodeGeneration: snapshot.allocation.descriptorLifecycleGeneration,
        objectGeneration: snapshot.objectGeneration,
        authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
        leaseGeneration: snapshot.ownerLeaseGeneration,
        ownerId: snapshot.ownerId,
        storeVersion: snapshot.storeVersion.value
      };
    }

    const drops = () =>
      telemetry.records.filter(
        (record) =>
          record.eventId === 'zlink.dispatch_error' &&
          record.meshName === mesh &&
          record.spotId === spotId &&
          record.action === 'drop'
      );
    await telemetry.observed();
    const previous = await start('previous');
    class Notice {
      constructor(marker) {
        this.marker = marker;
      }
    }
    await previous.app
      .get(nest.ZLINK_SPOT_OUTBOUND)
      .sendToSpot(spotId, new Notice('create'))
      .instanceSpot('room')
      .inMesh(mesh)
      .submit();
    await created.promise;
    const runtime = previous.host.serviceRelocation;
    const oldRoute = snapshotRoute(await runtime.options.locationStore().readAuthority(key));
    assert.equal(oldRoute.targetNodeRid, previous.rid);
    const current = await start('target', previous);
    const caller = await start(callerRid, previous);
    while (
      !previous.node.stateful.raw.isPeerRouteReady(current.rid) ||
      !current.node.stateful.raw.isPeerRouteReady(previous.rid) ||
      !caller.node.stateful.raw.isPeerRouteReady(previous.rid)
    ) {
      await delay(10, undefined, { signal: t.signal });
    }

    const targetRuntime = current.host.serviceRelocation;
    const progress = targetRuntime.targetProgress.bind(targetRuntime);
    t.mock.method(targetRuntime, 'targetProgress', async (stage, authority, phase, signal) => {
      if (phase === 'activating' && stage.staging.hidden.has(key.value)) {
        committed.resolve(stage);
        await publish.promise;
      }
      return progress(stage, authority, phase, signal);
    });

    const validFollow = deferred();
    const invalidFollow = deferred();
    const ingress = current.node.stateful.ingress.bind(current.node.stateful);
    t.mock.method(current.node.stateful, 'ingress', async (record) => {
      const result = await ingress(record);
      if (record.command === M6bServiceWireCommand.instanceSpotFollow) {
        const follow = decodeInstanceSpotFollowCommand(record.parts, context);
        const receipt =
          Buffer.from(follow.sourceNodeRid).toString() === caller.rid ? validFollow : invalidFollow;
        receipt.resolve(result);
      }
      return result;
    });

    moving = previous.host.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 15_000
    });
    let validResult;
    let validDrops;
    let stage;
    try {
      stage = await Promise.race([
        committed.promise,
        moving.then((result) => {
          assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
          return committed.promise;
        })
      ]);
      const snapshot = await targetRuntime.options.locationStore().readAuthority(key);
      const newRoute = snapshotRoute(snapshot);
      assert.equal(snapshot.relocation.phase, 'committed');
      assert.equal(newRoute.targetNodeRid, current.rid);
      assert.equal(newRoute.objectGeneration, oldRoute.objectGeneration);
      assert.equal((await moving).outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);

      assert.equal(
        await caller.node.sendToInstanceSpot(
          oldRoute,
          encodeChannelEnvelopeParts(3, mesh, 'Notice', new Notice('follow'))
        ),
        SubmitResult.Ok
      );
      validResult = await validFollow.promise;
      await telemetry.observed();
      validDrops = drops().length;

      assert.equal(
        await previous.node.stateful.raw.sendApplication(
          current.rid,
          followParts(
            { ...newRoute, authorityOwnerGeneration: newRoute.authorityOwnerGeneration + 1n },
            1,
            {
              ...application,
              payload: encodeMultipartPayload(
                encodeChannelEnvelopeParts(3, mesh, 'Notice', new Notice('wrong-fence'))
              )
            }
          ),
          newRoute.targetNodeGeneration
        ),
        true
      );
      assert.equal(await invalidFollow.promise, 'dropped');
      await telemetry.observed();
      assert.equal(drops().length - validDrops, 1, 'wrong target generation must produce one drop');
      assert.equal(drops().at(-1).reason, 'stale_target');
      assert.equal(drops().at(-1).sourceRid, original.rid);
      assert.deepEqual(handled, { previous: [], target: [] }, 'handlers must wait for publication');
    } finally {
      publish.resolve();
    }

    await stage.finalize;
    if (validResult === 'application' || validResult === 'infrastructure') await delivered.promise;
    await current.node.waitForInstanceApplicationQuiescence(spotId);
    await telemetry.observed();
    assert.deepEqual(
      { previous: handled.previous, target: handled.target, drops: validDrops },
      { previous: [], target: [{ marker: 'follow' }], drops: 0 }
    );
    assert.equal(drops().length, 1);
  }
);

test('invalid Ready Follow routes drop once without handler admission', async (t) => {
  for (const scenario of [
    'missing',
    'expired',
    'hop limit',
    'old source generation',
    'old target generation'
  ]) {
    await t.test(scenario, async (t) => {
      const previous = fixture(t, source);
      const current = fixture(t, target);
      let receiver = previous;
      let peer = original;
      let parts = readyParts();
      if (scenario === 'missing') {
        await previous.relocate(null);
      } else if (scenario === 'expired') {
        await previous.relocate(target, 1);
        await new Promise((resolve) => setTimeout(resolve, 10));
      } else if (scenario === 'hop limit') {
        await previous.relocate(target);
        parts = followParts(source, 8);
        peer = previousPeer;
      } else if (scenario === 'old source generation') {
        await previous.relocate(target);
        parts = readyParts({ ...source, objectGeneration: source.objectGeneration - 1n });
      } else {
        receiver = current;
        parts = followParts({ ...target, objectGeneration: target.objectGeneration - 1n }, 1);
        peer = previousPeer;
      }
      assert.equal(await receiver.receive(parts, peer), 'dropped');
      assert.equal(receiver.diagnostics.length, 1);
      assert.equal(receiver.diagnostics[0].sourceRid, original.rid);
      assert.equal(previous.pending() + current.pending(), 0);
      assert.equal(previous.sent.length + current.sent.length, 0);
    });
  }
});

test('the generated codec and service ingress reject command 55 with hop zero', async (t) => {
  const parts = followParts(target, 1);
  const hopOffset = 5 + 8 + 8 + 8 + 1 + Buffer.byteLength(original.rid) + 1;
  parts[0][hopOffset] = 0;
  assert.throws(() => decodeInstanceSpotFollowCommand(parts, context), /messageFollowHopCount/);
  const current = fixture(t, target);
  assert.equal(await current.receive(parts, previousPeer), 'protocolError');
  assert.equal(current.pending(), 0);
});
