const { observeDiagnosticSubmissions } = require('./helpers/diagnostic-submissions.cjs');
'use strict';

const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { Message } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  opaqueRecordPreimage
} = require('../../packages/framework/dist/runtime/locations/opaque-record-key');
const {
  encodeStreamHeader,
  ZLinkStreamMessageKind,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags
} = require('../../packages/framework/dist/runtime/streams/protocol');
const nest = require('../../packages/nestjs/dist');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const deferred = require('./helpers/deferred.cjs');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');
const telemetry = require('./helpers/telemetry-log-capture');

test(
  'remote Deferred Join reports Unavailable to an unsettled source Actor at the stop deadline',
  { timeout: 15_000 },
  async (t) => {
    await telemetry.observed();
    const clock = fixedRidHostClock(t);
    const mesh = 'join-stop-' + randomUUID();
    const actorId = 'player';
    const roomId = 'target-room';
    const key = encodeAuthorityKey('actor', actorId);
    const provider = new ZLinkInMemoryProviderLocationStore();
    const relocationStore = new MemoryRelocationStore();
    const settlementStarted = deferred();
    const releaseSettlement = deferred();
    const settlementFinished = deferred();
    const targetJoined = deferred();
    const sourceJoined = deferred();
    const completions = [];
    const apps = [];
    t.after(async () => {
      releaseSettlement.resolve();
      for (const app of apps.reverse()) await app.close();
    });

    class Player {
      constructor(context, role) {
        this.context = context;
        this.role = role;
      }

      onJoinCompleted(completion) {
        completions.push({ role: this.role, completion });
        if (this.role === 'target') targetJoined.resolve({ actor: this, completion });
        else sourceJoined.resolve(completion);
      }
    }
    class Factory {
      constructor(role) {
        this.role = role;
      }

      async create(context) {
        return new Player(context, this.role);
      }
    }
    class Entry {
      onCreateActor() {
        return { accepted: true };
      }

      onJoinedActor() {}

      onLeaveActor() {}
    }
    class Room {
      onCreate() {
        return { accepted: true };
      }

      onActorJoin() {
        return { accepted: true };
      }

      onJoinedActor() {}

      onLeaveActor() {}
    }
    class Move {
      handle(_spot, actor) {
        actor.context.joinSpot(roomId).defer();
        return { queued: true };
      }
    }
    for (const Type of [Entry, Room, Move]) Injectable()(Type);
    nest.zlinkEntrySpotActorRequestHandler({
      actor: () => Player,
      entrySpot: () => Entry,
      packetName: 'Move'
    })(Move);

    async function start(role) {
      const builder = nest
        .zlinkFramework()
        .addLocationStore(provider)
        .addRelocationStore(relocationStore);
      builder.configureDispatch().messageFlow('normal');
      builder.configureLocations().ownerLeaseFencingMarginMs(1).pollingIntervalMs(20);
      const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(role);
      route.channel(mesh).server();
      const objects = route.objects().server();
      objects.addEntrySpot(Entry, 'Player');
      objects.addActorFactory('Player', Factory, (factory) => factory.recreateOnRelocation());
      if (role === 'target') {
        objects.addSpotFactory('Room', Room, (factory) => factory.disableRelocation());
      }
      class App {}
      Module({
        imports: [nest.ZLinkModule.forRoot(builder.build())],
        providers: [Entry, Room, Move, { provide: Factory, useValue: new Factory(role) }]
      })(App);
      const app = await NestFactory.createApplicationContext(App, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      await finishFixedRidHostStartup(host, clock, framework.ZLinkFrameworkRuntimeState.Preparing);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      return {
        app,
        host,
        actors: app.get(nest.ZLINK_ACTOR_MANAGER)
      };
    }

    const source = await start('source');
    const created = await source.actors.getOrCreate(actorId, 'Player').inMesh(mesh).submit();
    assert.equal(created.status, 'created');
    const target = await start('target');
    for (const { app } of [source, target]) {
      for await (const snapshot of app
        .get(nest.ZLINK_ROUTE_MESH_RUNTIME)
        .observe(mesh, 64, t.signal)) {
        if (snapshot.status.isReady && snapshot.status.readyPeerCount === 1) break;
      }
    }
    const room = await target.app
      .get(nest.ZLINK_SPOT_MANAGER)
      .getOrCreate(roomId, 'Room')
      .inMesh(mesh)
      .submit();
    assert.equal(String(room.spot.nodeRid), 'target');

    const runtime = source.host.serviceRelocation;
    const relocate = t.mock.method(runtime, 'relocateActorJoin');
    const settle = runtime.settleSourceAuthority.bind(runtime);
    t.mock.method(runtime, 'settleSourceAuthority', async (...args) => {
      try {
        return await settle(...args);
      } finally {
        settlementFinished.resolve();
      }
    });
    const readSettlement = runtime.readSourceSettlement.bind(runtime);
    t.mock.method(runtime, 'readSourceSettlement', async (...args) => {
      settlementStarted.resolve();
      await releaseSettlement.promise;
      return readSettlement(...args);
    });
    const store = runtime.options.locationStore();
    const read = store.readAuthority.bind(store);
    const postDeadlineStoreCalls = [];
    let stopDeadline;
    let sourceReadFailures = 0;
    const readFailure = new Error('Source settlement Store read failed');
    t.mock.method(store, 'readAuthority', async (authorityKey, ...args) => {
      if (authorityKey.value === key.value && stopDeadline !== undefined) {
        if (stopDeadline.aborted) postDeadlineStoreCalls.push('read');
        assert.equal(runtime.options.shutdownRequested().aborted, true);
        sourceReadFailures += 1;
        throw readFailure;
      }
      return read(authorityKey, ...args);
    });
    const rowKey = { value: opaqueRecordPreimage('authority', 'actor', actorId) };
    const readRow = provider.read.bind(provider);
    t.mock.method(provider, 'read', (requestedKey, ...args) => {
      if (requestedKey.value === rowKey.value && stopDeadline?.aborted) {
        postDeadlineStoreCalls.push('read');
      }
      return readRow(requestedKey, ...args);
    });
    const write = provider.write.bind(provider);
    t.mock.method(provider, 'write', (request, ...args) => {
      if (
        stopDeadline?.aborted &&
        request.mutations.some(({ key }) => key.value === rowKey.value)
      ) {
        postDeadlineStoreCalls.push('write');
      }
      return write(request, ...args);
    });

    assert.deepEqual(
      await source.app
        .get(nest.ZLINK_ACTOR_CLIENT)
        .requestToActor(actorId, {})
        .packetName('Move')
        .submit(),
      { queued: true }
    );
    await Promise.race([
      settlementStarted.promise,
      sourceJoined.promise.then((completion) => {
        assert.fail(
          `Join completed before source settlement: ${require('node:util').inspect(completion)}`
        );
      })
    ]);
    const { actor: targetActor, completion: accepted } = await targetJoined.promise;
    assert.equal(accepted.status, 'accepted');
    assert.equal(String(accepted.actor.nodeRid), 'target');
    await target.host.serviceRelocation.drainTargetActorRelocation(targetActor);
    const targetAuthority = await read(key);
    assert.equal(targetAuthority.kind, 'snapshot');
    assert.equal(
      (
        await write({
          conditions: [{ kind: 'version', key: rowKey, expected: targetAuthority.storeVersion }],
          mutations: [{ kind: 'delete', key: rowKey }]
        })
      ).kind,
      'applied'
    );
    assert.equal((await read(key)).kind, 'missing');

    // Append after the cutover boundary, so the source still owns this backlog record.
    const handoff = source.host.actorHandoff;
    const parts = [
      Message.from(
        encodeStreamHeader({
          kind: ZLinkStreamMessageKind.Request,
          codec: ZLinkStreamCodec.Json,
          flags: ZLinkStreamHeaderFlags.None,
          requestSeq: 1n,
          name: 'Backlog',
          metadata: new Map()
        })
      ),
      Message.from('{}')
    ];
    const admitted = deferred();
    let backlogSettled = false;
    try {
      const captured = handoff.capture(
        'local',
        actorId,
        parts,
        true,
        undefined,
        created.actor,
        undefined,
        undefined,
        undefined,
        admitted.resolve
      );
      assert.ok(captured);
      const markSettled = () => {
        backlogSettled = true;
      };
      await Promise.race([admitted.promise, captured.then(markSettled, markSettled)]);
      assert.equal(
        backlogSettled,
        false,
        'backlog request must be admitted while settlement is held'
      );
    } finally {
      parts.forEach((part) => part.close());
    }

    const deadlineDiagnostics = [];
    const debug = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '1';
    const observeDeadline = observeDiagnosticSubmissions(({ marker, message, detail }) => {
      if (marker === '[zlink.runtime.relocation]' && message === 'shutdown_deadline_exceeded') {
        deadlineDiagnostics.push(detail);
      }
    }, t.mock);
    let stopped;
    try {
      const stopping = source.host.shutdown({ deadlineMs: 400 });
      stopDeadline = runtime.options.shutdownDeadline();
      assert.ok(stopDeadline);
      assert.equal(stopDeadline.aborted, false);
      releaseSettlement.resolve();
      stopped = await stopping;
      await settlementFinished.promise;
    } finally {
      observeDeadline.mock.restore();
      if (debug === undefined) delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
      else process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = debug;
    }

    assert.equal(stopped.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
    assert.equal(stopped.reason, framework.ZLinkFrameworkTerminationReason.DeadlineExceeded);
    assert.equal(stopDeadline.aborted, true);
    assert.ok(
      sourceReadFailures > 0,
      'source settlement must encounter Store failures during shutdown'
    );
    assert.equal(relocate.mock.callCount(), 1);
    const [
      {
        arguments: [{ completionOperationId }]
      }
    ] = relocate.mock.calls;
    assert.ok(completionOperationId);
    assert.deepEqual(completions, [
      { role: 'target', completion: accepted },
      {
        role: 'source',
        completion: {
          status: 'failed',
          kind: framework.ZLinkFrameworkErrorKind.Unavailable,
          operationId: completionOperationId
        }
      }
    ]);
    assert.deepEqual(postDeadlineStoreCalls, []);
    const backlog = handoff.snapshot(actorId);
    assert.equal(backlog.length, 1, 'source backlog must remain queued');
    assert.ok(backlog[0].source);
    assert.equal(backlogSettled, false, 'backlog request must keep its pending terminal');
    assert.equal((await readRow(rowKey)).kind, 'missing');
    assert.equal(deadlineDiagnostics.length, 1);
    assert.ok(
      deadlineDiagnostics[0].pendingSteps.every(
        ({ stage }) => stage !== 'owner_cleanup.spots' && stage !== 'transport_cleanup.spot_nodes'
      ),
      'Spot drain must not be pending at the stop deadline'
    );
    t.diagnostic(
      'source callbacks=1 (Failed/Unavailable), post-deadline Store calls=0, backlog=1, Spot drain pending=0'
    );
  }
);
