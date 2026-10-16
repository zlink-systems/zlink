if (process.env.ZLINK_JOIN_TEST_FLOW_FILE) {
  const fs = require('node:fs');
  const { logs } = require('@opentelemetry/api-logs');
  const { LoggerProvider, SimpleLogRecordProcessor } = require('@opentelemetry/sdk-logs');
  logs.setGlobalLoggerProvider(
    new LoggerProvider({
      processors: [
        new SimpleLogRecordProcessor({
          exporter: {
            export(records, done) {
              for (const record of records)
                fs.appendFileSync(
                  process.env.ZLINK_JOIN_TEST_FLOW_FILE,
                  JSON.stringify({ body: record.body, ...record.attributes }) + '\n'
                );
              done({ code: 0 });
            },
            async shutdown() {},
            async forceFlush() {}
          }
        })
      ]
    })
  );
}

const assert = require('node:assert/strict');
const test = require('node:test');
const PUBLIC_OPERATION_TIMEOUT_MS = 5_000;
const OBSERVATION_INTERVAL_MS = 10;
const RELOCATION_TEST_TIMEOUT_MS = 30_000;
const SOURCE_FOLLOW_DURATION_MS = 5_000;
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  decodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  ZLinkActorLocationClaims
} = require('../../packages/framework/dist/runtime/locations/actor-location-claims');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeServiceUserSpotAuthorityPayload
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
// Creation and Join use public managers, typed dispatch and completion callbacks.
// The Store contract assertion checks membership, which the public object query
// does not expose (it only returns the object's owner and generation).
test(
  'placement-created same-node Player commits Table and Entry membership before Accepted',
  { timeout: 15000 },
  async () => {
    const completions = [];
    let completed;
    class Join {
      constructor(spotId) {
        this.spotId = spotId;
      }
    }
    class Player {
      constructor(context) {
        this.context = context;
      }
      async onJoinCompleted(completion) {
        completions.push(completion);
        completed(completion);
      }
    }
    class PlayerFactory {
      async create(context) {
        return new Player(context);
      }
    }
    class Entry {
      constructor(context) {
        this.context = context;
      }
      configure() {}
    }
    class Table {
      constructor(context) {
        this.context = context;
      }
      configure() {}
      async onActorJoin() {
        return { accepted: true };
      }
      async onJoinedActor() {}
      async onLeaveActor() {}
    }
    class JoinHandler {
      async handle(_spot, actor, _context, request) {
        if (request.spotId === undefined) actor.context.joinEntrySpot().defer();
        else actor.context.joinSpot(request.spotId).defer();
      }
    }
    nestjs.zlinkEntrySpotActorSendHandler({
      entrySpot: () => Entry,
      actor: () => Player,
      packetName: 'Join'
    })(JoinHandler);
    class LeaveHandler extends JoinHandler {}
    nestjs.zlinkSpotActorSendHandler({
      spot: () => Table,
      actor: () => Player,
      packetName: 'Join'
    })(LeaveHandler);
    const provider = new ZLinkInMemoryProviderLocationStore();
    const builder = nestjs.zlinkFramework().addLocationStore(provider);
    builder
      .addRouteMesh('play')
      .listen(`inproc://join-authority-${process.pid}`)
      .objects()
      .server()
      .addEntrySpot(Entry)
      .addActorFactory('Player', PlayerFactory, (f) => f.disableRelocation())
      .addSpotFactory('Table', Table, (f) => f.disableRelocation());
    class TestModule {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [JoinHandler, LeaveHandler]
    })(TestModule);
    const app = await NestFactory.createApplicationContext(TestModule, {
      logger: false,
      abortOnError: false
    });
    try {
      const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
      const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
      const client = app.get(nestjs.ZLINK_ACTOR_CLIENT);
      const query = app.get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY);
      const table = await spots.getOrCreate('table', 'Table').inMesh('play').submit();
      await actors.create('player', 'Player').inMesh('play').submit();
      const repository = new ZLinkLocationStoreRepository(provider);
      const key = encodeAuthorityKey('actor', 'player');
      const before = await repository.readAuthority(key);
      const entry = decodeActorAuthorityIdentity(before.payload, before.objectGeneration);
      for (const [spotId, generation, kind] of [
        [table.spot.spotId, table.spot.objectGeneration, framework.ZLinkSpotKind.User],
        [entry.spotId, entry.spotGeneration, framework.ZLinkSpotKind.Entry]
      ]) {
        const completion = new Promise((resolve) => {
          completed = resolve;
        });
        await client
          .sendToActor(
            'player',
            new Join(kind === framework.ZLinkSpotKind.User ? String(spotId) : undefined)
          )
          .submit();
        assert.equal((await completion).status, 'accepted');
        const location = await query.findActorLocation('player');
        assert.equal(location.objectGeneration, before.objectGeneration);
        const membership = await actors.findSpot('player');
        if (kind === framework.ZLinkSpotKind.User) {
          assert.equal(String(membership.spotId), String(spotId));
          assert.equal(
            (await query.findSpotLocation(membership.spotId)).objectGeneration,
            generation
          );
        } else {
          assert.equal(membership, undefined);
        }
        const current = await repository.readAuthority(key);
        const identity = decodeActorAuthorityIdentity(current.payload, current.objectGeneration);
        assert.equal(String(identity.spotId), String(spotId));
        assert.equal(identity.spotGeneration, generation);
        assert.equal(identity.spotKind, kind);
      }
      assert.equal(completions.length, 2);
    } finally {
      await app.close();
    }
  }
);

function authorityFixture() {
  const actor = { actorId: 'player', objectGeneration: 3n, nodeRid: 'node', meshName: 'play' };
  const owner = { ownerId: 'owner', leaseGeneration: 2n };
  const identity = {
    actor,
    actorType: 'Player',
    meshName: 'play',
    owner,
    ownerNodeGeneration: 7n,
    spotId: 'entry',
    spotGeneration: 7n,
    spotKind: framework.ZLinkSpotKind.Entry
  };
  const snapshot = {
    kind: 'snapshot',
    objectGeneration: 3n,
    authorityOwnerGeneration: 5n,
    ownerId: owner.ownerId,
    ownerLeaseGeneration: owner.leaseGeneration,
    storeVersion: { value: 'v1' },
    storeNow: new Date(0),
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'play', rid: 'node' },
      descriptorLifecycleGeneration: 7n,
      capacity: { actors: 1, spots: 0 }
    },
    payload: encodeActorAuthorityIdentity(identity)
  };
  return { actor, owner, identity, snapshot };
}

for (const variant of [
  'same-node',
  'cross-node',
  'relocated Entry',
  'safe status',
  'Spot generation',
  'target shutdown',
  'Session relocation',
  'Session shutdown'
]) {
  test(
    `${variant} public Join and PlannedMaintenance preserve canonical Table inventory`,
    { timeout: RELOCATION_TEST_TIMEOUT_MS },
    async () => {
      const provider = new ZLinkInMemoryProviderLocationStore();
      const restored = [];
      const blobs = new Map();
      const relocationStore = {
        async put(reference, bytes, retentionMs) {
          const storeNow = new Date();
          const value = {
            bytes: Buffer.from(bytes),
            storeNow,
            expiresAt: new Date(storeNow.getTime() + retentionMs)
          };
          blobs.set(reference.value, value);
          return { kind: 'stored', ...value };
        },
        async read(reference) {
          return blobs.has(reference.value)
            ? { kind: 'found', ...blobs.get(reference.value) }
            : { kind: 'missing', storeNow: new Date() };
        },
        async renew(reference, retentionMs) {
          const value = blobs.get(reference.value);
          const storeNow = new Date();
          if (value === undefined) return { kind: 'missing', storeNow };
          value.expiresAt = new Date(storeNow.getTime() + retentionMs);
          return { kind: 'renewed', storeNow, expiresAt: value.expiresAt };
        },
        async delete(reference) {
          blobs.delete(reference.value);
        }
      };
      let complete;
      class Move {
        constructor(spotId) {
          this.spotId = spotId;
        }
      }
      class Player {
        constructor(context) {
          this.context = context;
        }
        async onJoinCompleted(value) {
          complete(value);
        }
      }
      class PlayerFactory {
        async create(context) {
          return new Player(context);
        }
      }
      class PlayerState {
        async capture() {
          return new Uint8Array();
        }
        async restore(actor) {
          restored.push(`Player:${actor.context.actorId}`);
        }
      }
      class Entry {
        constructor(context) {
          this.context = context;
        }
        configure() {}
      }
      class Table {
        constructor(context) {
          this.context = context;
        }
        configure() {}
        async onActorJoin() {
          return { accepted: true };
        }
        async onJoinedActor() {}
        async onLeaveActor() {}
      }
      class TableState {
        async capture() {
          return new Uint8Array();
        }
        async restore(spot) {
          restored.push(`Table:${spot.context.spotId}`);
        }
      }
      class MoveHandler {
        async handle(_entry, actor, _context, request) {
          actor.context.joinSpot(request.spotId).defer();
        }
      }
      nestjs.zlinkEntrySpotActorSendHandler({
        entrySpot: () => Entry,
        actor: () => Player,
        packetName: 'Move'
      })(MoveHandler);
      const apps = [];
      let client;
      let sessionActor;
      class SessionFactory {
        async create(context) {
          return {
            context,
            async onDispatch(dispatch) {
              if (dispatch.packetName === 'Bind') await context.actors.bindOrGet(sessionActor);
              await context.client
                .reply({ nodeRid: String(context.actors.find('player')?.ref.nodeRid) })
                .submit();
            }
          };
        }
      }
      const localTable = variant !== 'cross-node' && variant !== 'relocated Entry';
      async function start(node) {
        const builder = nestjs
          .zlinkFramework()
          .addLocationStore(provider)
          .addRelocationStore(relocationStore);
        if (process.env.ZLINK_JOIN_TEST_FLOW_FILE)
          builder.configureDispatch().messageFlow('normal');
        const locations = builder
          .configureLocations()
          .pollingIntervalMs(OBSERVATION_INTERVAL_MS)
          .routeCacheMaxAgeMs(0);
        if (variant === 'safe status') locations.messageFollowDurationMs(SOURCE_FOLLOW_DURATION_MS);
        const mesh = builder.addRouteMesh('play').listen('tcp://127.0.0.1:0').routingId(node);
        if (node === 'session') mesh.objects().client();
        else
          mesh
            .objects()
            .server()
            .addEntrySpot(Entry)
            .addActorFactory('Player', PlayerFactory, (f) => f.preserveStateWith(PlayerState))
            .addSpotFactory('Table', Table, (f) => f.preserveStateWith(TableState));
        class TestModule {}
        if (variant.startsWith('Session') && node === 'session') {
          builder
            .addStreamNode('sessions')
            .enableActorDispatch()
            .bind('ws://127.0.0.1:*')
            .registerSession(SessionFactory);
        }
        Module({
          imports: [nestjs.ZLinkModule.forRoot(builder.build())],
          providers: node === 'session' ? [SessionFactory] : [MoveHandler, PlayerState, TableState]
        })(TestModule);
        const app = await NestFactory.createApplicationContext(TestModule, {
          logger: false,
          abortOnError: false
        });
        apps.push(app);
        return app;
      }
      try {
        const source = await start('source');
        const actors = source.get(nestjs.ZLINK_ACTOR_MANAGER);
        const query = source.get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY);
        await actors.create('player', 'Player').inMesh('play').submit();
        let table;
        if (localTable)
          table = await source
            .get(nestjs.ZLINK_SPOT_MANAGER)
            .getOrCreate('table', 'Table')
            .inMesh('play')
            .submit();
        const target = await start('target');
        source.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME_OPTIONS).mesh('play').placementWeight = 0;
        const deadline = Date.now() + PUBLIC_OPERATION_TIMEOUT_MS;
        while (
          (await query.listMeshNodeDescriptors('play')).items.find(
            (d) => String(d.rid) === 'source'
          )?.placementWeight !== 0 ||
          source.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount < 1 ||
          target.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount < 1
        ) {
          assert.ok(Date.now() < deadline, 'placement publication and public peer readiness');
          await new Promise((resolve) => setTimeout(resolve, OBSERVATION_INTERVAL_MS));
        }
        let owner = source;
        if (variant === 'relocated Entry') {
          const terminal = await source.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).relocate({
            mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
            deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS
          });
          assert.equal(terminal.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
          assert.ok(restored.includes('Player:player'));
          assert.equal(
            String(
              (await target.get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY).findActorLocation('player'))
                .nodeRid
            ),
            'target'
          );
          owner = target;
        }
        if (table === undefined)
          table = await target
            .get(nestjs.ZLINK_SPOT_MANAGER)
            .getOrCreate('table', 'Table')
            .inMesh('play')
            .submit();
        if (variant.startsWith('Session')) {
          const gateway = await start('session');
          const sessionDeadline = Date.now() + PUBLIC_OPERATION_TIMEOUT_MS;
          while (gateway.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount < 2) {
            assert.ok(Date.now() < sessionDeadline, 'Session owner peer readiness');
            await new Promise((resolve) => setTimeout(resolve, OBSERVATION_INTERVAL_MS));
          }
          sessionActor = await actors.find('player');
          client = connector.zlinkStreamConnectorFactory.create({
            endpoint: gateway
              .get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
              .getListenerStatus('stream', 'sessions').endpoint,
            dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
          });
          await client.connect();
          assert.deepEqual(await client.request({}).packetName('Bind').submit(), {
            nodeRid: 'source'
          });
        }
        const completion = new Promise((resolve) => {
          complete = resolve;
        });
        await owner
          .get(nestjs.ZLINK_ACTOR_CLIENT)
          .sendToActor('player', new Move('table'))
          .submit();
        assert.equal((await completion).status, 'accepted');
        const repository = new ZLinkLocationStoreRepository(provider);
        const current = await repository.readAuthority(encodeAuthorityKey('actor', 'player'));
        const identity = decodeActorAuthorityIdentity(current.payload, current.objectGeneration);
        assert.equal(identity.spotId, 'table');
        assert.equal(identity.spotGeneration, table.spot.objectGeneration);
        assert.equal(String(identity.actor.nodeRid), localTable ? 'source' : 'target');
        if (variant === 'Spot generation') {
          assert.equal(
            (await actors.findSpot('player')).objectGeneration,
            table.spot.objectGeneration
          );
        }
        if (localTable) {
          const runtime = source.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
          if (variant === 'Session shutdown') {
            assert.deepEqual(await runtime.shutdown({ deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS }), {
              outcome: framework.ZLinkFrameworkTerminationOutcome.Stopped,
              reason: framework.ZLinkFrameworkTerminationReason.None
            });
            return;
          }
          const observation = new AbortController();
          const statuses = [];
          const observer =
            variant === 'safe status'
              ? (async () => {
                  for await (const event of runtime.observe(observation.signal)) {
                    statuses.push(event.status);
                  }
                })()
              : Promise.resolve();
          try {
            const terminal = await source.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).relocate({
              mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
              deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS
            });
            assert.equal(terminal.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
            assert.ok(restored.includes('Table:table'));
            assert.ok(restored.includes('Player:player'));
            if (variant === 'Session relocation') {
              const routeDeadline = Date.now() + PUBLIC_OPERATION_TIMEOUT_MS;
              while (
                (await client.request({}).packetName('Status').submit()).nodeRid !== 'target'
              ) {
                assert.ok(Date.now() < routeDeadline, 'Session route observes the relocated Actor');
                await new Promise((resolve) => setTimeout(resolve, OBSERVATION_INTERVAL_MS));
              }
              assert.deepEqual(
                await target
                  .get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
                  .shutdown({ deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS }),
                {
                  outcome: framework.ZLinkFrameworkTerminationOutcome.Stopped,
                  reason: framework.ZLinkFrameworkTerminationReason.None
                }
              );
            }
            if (variant === 'target shutdown') {
              assert.deepEqual(
                await target.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).shutdown({
                  deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS
                }),
                {
                  outcome: framework.ZLinkFrameworkTerminationOutcome.Stopped,
                  reason: framework.ZLinkFrameworkTerminationReason.None
                }
              );
            }
            if (variant === 'safe status') {
              const observationDeadline = Date.now() + PUBLIC_OPERATION_TIMEOUT_MS;
              while (!runtime.status.safeToShutdown && Date.now() < observationDeadline) {
                await new Promise((resolve) => setTimeout(resolve, OBSERVATION_INTERVAL_MS));
              }
              assert.equal(runtime.status.safeToShutdown, true, 'source quiescence is queryable');
              assert.ok(
                statuses.some(
                  (status) =>
                    status.state === framework.ZLinkFrameworkRuntimeState.Relocated &&
                    status.safeToShutdown
                ),
                'SAFE_TO_SHUTDOWN must be published after source quiescence changes'
              );
              const published = statuses.findLast(
                (status) =>
                  status.safeToShutdown &&
                  status.state === framework.ZLinkFrameworkRuntimeState.Relocated
              );
              assert.equal(published.sequence, runtime.status.sequence);
              assert.equal(published, runtime.status);
              assert.ok(
                statuses.some(
                  (status) => !status.safeToShutdown && status.sequence < published.sequence
                )
              );
            }
          } finally {
            observation.abort();
            await observer;
          }
        }
        if (variant === 'cross-node') {
          const destination = await start('restore');
          const peerDeadline = Date.now() + PUBLIC_OPERATION_TIMEOUT_MS;
          while (target.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount < 2) {
            assert.ok(Date.now() < peerDeadline, 'relocation destination peer readiness');
            await new Promise((resolve) => setTimeout(resolve, OBSERVATION_INTERVAL_MS));
          }
          const restoreStart = restored.length;
          const terminal = await target.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).relocate({
            mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
            deadlineMs: PUBLIC_OPERATION_TIMEOUT_MS
          });
          assert.equal(terminal.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
          assert.ok(restored.slice(restoreStart).includes('Table:table'));
          assert.ok(restored.slice(restoreStart).includes('Player:player'));
          assert.equal(
            String(
              (
                await destination
                  .get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY)
                  .findActorLocation('player')
              ).nodeRid
            ),
            'restore'
          );
        }
      } finally {
        await client?.close();
        for (const app of apps.reverse()) await app.close();
      }
    }
  );
}

for (const mismatch of [
  'missing',
  'object',
  'authorityOwner',
  'owner',
  'lease',
  'node',
  'nodeGeneration',
  'identity'
]) {
  test(`local membership rejects ${mismatch} authority before Store CAS`, async () => {
    const f = authorityFixture();
    let snapshot = f.snapshot;
    if (mismatch === 'missing') snapshot = { kind: 'missing' };
    if (mismatch === 'object') snapshot.objectGeneration++;
    if (mismatch === 'authorityOwner') snapshot.authorityOwnerGeneration++;
    if (mismatch === 'owner') snapshot.ownerId = 'other';
    if (mismatch === 'lease') snapshot.ownerLeaseGeneration++;
    if (mismatch === 'node') snapshot.allocation.descriptor.rid = 'other';
    if (mismatch === 'nodeGeneration') snapshot.allocation.descriptorLifecycleGeneration++;
    if (mismatch === 'identity')
      snapshot.payload = encodeActorAuthorityIdentity({ ...f.identity, ownerNodeGeneration: 8n });
    let writes = 0;
    const claims = new ZLinkActorLocationClaims({ currentOwnerToken: f.owner }, {}, 'play', {
      async readAuthority() {
        return snapshot;
      },
      async compareExchangeAuthority() {
        writes++;
        throw new Error('unexpected CAS');
      }
    });
    await assert.rejects(
      () =>
        claims.commitMembership(
          'Player',
          f.actor,
          5n,
          'table',
          11n,
          framework.ZLinkSpotKind.User,
          7n
        ),
      /authority (fence|identity) does not match/
    );
    assert.equal(writes, 0);
  });
}

test('untracked local membership uses one exact CAS and reports Store conflict', async () => {
  const f = authorityFixture();
  const writes = [];
  const claims = new ZLinkActorLocationClaims({ currentOwnerToken: f.owner }, {}, 'play', {
    async readAuthority(key) {
      assert.deepEqual(key, encodeAuthorityKey('actor', 'player'));
      return f.snapshot;
    },
    async compareExchangeAuthority(key, version, mutation) {
      writes.push({ key, version, mutation });
      return { kind: 'conflict', current: f.snapshot };
    }
  });
  await assert.rejects(
    () =>
      claims.commitMembership(
        'Player',
        f.actor,
        5n,
        'table',
        11n,
        framework.ZLinkSpotKind.User,
        7n
      ),
    /commit.*conflict/
  );
  assert.equal(writes.length, 1);
  assert.equal(writes[0].version, f.snapshot.storeVersion);
  const candidate = decodeActorAuthorityIdentity(writes[0].mutation.payload, 3n);
  assert.equal(candidate.spotId, 'table');
  assert.equal(candidate.spotGeneration, 11n);
  assert.deepEqual(candidate.actor, f.actor);
  assert.deepEqual(candidate.owner, f.owner);
});

for (const membership of [
  'current',
  'other Entry membership',
  'stale Spot incarnation',
  'stale root incarnation'
]) {
  test(`canonical relocation inventory validates ${membership}`, async () => {
    const f = authorityFixture();
    const primary = {
      ...f.snapshot,
      objectGeneration: 11n,
      authorityOwnerGeneration: 13n,
      allocation: {
        ...f.snapshot.allocation,
        objectKind: 'user_spot',
        stableType: 'Table',
        capacity: { actors: 0, spots: 1 }
      },
      payload: encodeServiceUserSpotAuthorityPayload({
        state: 'ready',
        stableType: 'Table',
        spotId: 'table',
        ownerId: f.owner.ownerId,
        ownerLeaseGeneration: f.owner.leaseGeneration,
        ownerMeshName: f.identity.meshName,
        ownerNodeRid: String(f.actor.nodeRid),
        ownerNodeGeneration: f.identity.ownerNodeGeneration
      })
    };
    if (membership !== 'other Entry membership') {
      f.snapshot.payload = encodeActorAuthorityIdentity({
        ...f.identity,
        spotId: 'table',
        spotKind: framework.ZLinkSpotKind.User,
        spotGeneration:
          membership === 'stale Spot incarnation'
            ? primary.objectGeneration - 1n
            : primary.objectGeneration
      });
    }
    const runtime = new ZLinkHostServiceRelocationRuntime({
      spotManager: () => ({}),
      locationStore: () => ({
        async readAuthority() {
          return primary;
        },
        async listAuthorities() {
          return {
            kind: 'page',
            items: [{ key: encodeAuthorityKey('actor', 'player'), snapshot: f.snapshot }]
          };
        }
      })
    });
    const root = {
      participantId: 2n,
      rootSpotId: 'table',
      rootObjectKind: 'user_spot',
      rootSpotGeneration:
        membership === 'stale root incarnation'
          ? primary.objectGeneration - 1n
          : primary.objectGeneration,
      rootOwnerGeneration: primary.authorityOwnerGeneration
    };
    const resolve = () =>
      runtime.resolveTargetParticipantInventory(
        'play',
        {
          object: { kind: 'userSpot', spotId: 'table', objectGeneration: primary.objectGeneration }
        },
        { participants: [{ participantId: 1n }, root] }
      );
    if (membership === 'current') {
      const result = await resolve();
      assert.equal(result.participants.length, 2);
      assert.equal(result.memberships.length, 1);
      assert.equal(result.memberships[0].spotObjectGeneration, primary.objectGeneration);
    } else {
      await assert.rejects(
        resolve,
        membership === 'stale root incarnation' ? /root object projection/ : /participant inventory/
      );
    }
  });
}
