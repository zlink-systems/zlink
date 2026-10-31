const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  ZLinkSpotRuntimeOptionsFactory
} = require('../../packages/framework/dist/runtime/host/spot-runtime-options-factory');
const {
  ZLinkSpotActivation
} = require('../../packages/framework/dist/runtime/spots/spot-activation-state');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkRuntimeTaskRunner,
  ZLinkRuntimeTaskErrorSink
} = require('../../packages/framework/dist/runtime/execution');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { Message, RequestResult } = require('@zlink-systems/zlink');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');

for (const [changedVersion, departed] of [
  [false, false],
  [false, true],
  [true, false],
  [true, true]
]) {
  test(`relocation membership shares target admission visibility (departed=${departed}, changedVersion=${changedVersion})`, async () => {
    const actorId = 'hidden-player';
    let authorityReads = 0;
    let cachedVersion = changedVersion ? 'v11' : 'v12';
    const routeSnapshot = () => ({
      kind: 'snapshot',
      storeVersion: { value: cachedVersion },
      payload: new Uint8Array(),
      objectGeneration: 7n,
      authorityOwnerGeneration: cachedVersion === 'v11' ? 11n : 12n,
      ownerId: 'owner',
      ownerLeaseGeneration: 1n,
      allocation: {
        state: 'active',
        objectKind: 'actor',
        stableType: 'player',
        descriptor: { meshName: 'play', rid: cachedVersion === 'v11' ? 'source' : 'target' },
        descriptorLifecycleGeneration: 1n
      }
    });
    const resolver = new framework.ZLinkStoreLocationResolvers({
      stores: {
        authorityStore: {
          async readAuthority() {
            authorityReads++;
            return routeSnapshot();
          }
        }
      },
      leaseTracker: {
        async remainingOwnerTokenLeaseMs() {
          return 60_000;
        }
      }
    });
    const retained = await resolver.resolveDirectActorRoute(actorId);
    cachedVersion = 'v12';

    class JoinedRequest {}
    let requests = 0;
    const operationId = { high: 1n, low: 1n };
    const client = new framework.DefaultZLinkActorClient({
      locationResolver: () => resolver,
      nodeProvider: () => ({
        requestToActor(actor, _parts, options) {
          assert.equal(String(actor.nodeRid), 'target');
          assert.equal(options.routeFence.authorityOwnerGeneration, 12n);
          requests++;
          return operationId;
        }
      }),
      completionTableProvider: () => ({
        submit(submit, _signal, operation, receiver) {
          const work = Promise.resolve().then(() => {
            assert.deepEqual(submit(), operationId);
            return {
              terminalResult: RequestResult.Ok,
              failureErrno: 0,
              operationKind: 0,
              kindData: null,
              parts: [
                Message.from(
                  Buffer.from(
                    framework.encodeStreamFrame(
                      {
                        kind: framework.ZLinkStreamMessageKind.Response,
                        codec: framework.ZLinkStreamCodec.Json,
                        flags: framework.ZLinkStreamHeaderFlags.None,
                        name: '',
                        metadata: new Map()
                      },
                      Buffer.from('"joined"')
                    )
                  )
                )
              ]
            };
          });
          void work.then(
            (value) => receiver.operationCompleted(operation.id, value, undefined),
            (error) => receiver.operationFailed(operation.id, error, undefined)
          );
        }
      })
    });
    const actorKey = encodeAuthorityKey('actor', actorId).value;
    const spotId = 'room';
    const spotKey = encodeAuthorityKey('user_spot', spotId).value;
    const nativeRef = { actorId, nodeRid: 'target', generation: 7n };
    const node = {
      restoreActorAuthority: () => nativeRef,
      actorLookup: () => ({ actor: nativeRef, spotId, spotGeneration: 5n, membershipEpoch: 3n }),
      discardRelocatedActor() {}
    };
    class Player {
      constructor(context) {
        this.context = context;
      }
    }
    const runner = new ZLinkRuntimeTaskRunner(
      new ZLinkRuntimeTaskErrorSink(),
      new AbortController().signal
    );
    const manager = new framework.DefaultZLinkActorManager(
      {
        actorFactories: new Map([['player', { create: (context) => new Player(context) }]]),
        nativeActorNode: node,
        actorMeshNameProvider: () => 'play'
      },
      runner
    );
    let handlers = 0;
    let releaseFirst;
    const firstTerminal = new Promise((resolve) => {
      releaseFirst = resolve;
    });
    const order = [];
    class Probe {
      async handle(_spot, actor, _context, packet) {
        const state = manager.getState(actor.context.actorId);
        // This is the committed source-fence input used by the next Deferred Join.
        assert.equal(state.ownerLeaseGeneration, 1n, 'handler must have a committed owner lease');
        assert.equal(state.locationGeneration, 12n);
        handlers++;
        order.push(packet.order);
        if (handlers === 1) await firstTerminal;
      }
    }
    const activation = new ZLinkSpotActivation({
      meshName: 'play',
      spotId,
      domain: {
        kind: 'user',
        executionMode: framework.ZLinkUserSpotExecutionMode.SpotWide,
        relocationCoordinationMode: framework.ZLinkSpotRelocationCoordinationMode.FrameworkManaged
      },
      spotType: class Room {},
      spot: {
        context: { meshName: 'play' },
        async onJoinedActor() {
          assert.equal(
            await client.requestToActor(actorId, new JoinedRequest()).submit(),
            'joined'
          );
          assert.equal(requests, 1);
          const result = await resolver.resolveDirectActorRoute(actorId);
          assert.equal(result.kind, 'ready');
          assert.equal(String(result.route.actorRef.nodeRid), 'target');
          assert.equal(result.route.authorityOwnerGeneration, 12n);
          assert.equal(
            authorityReads,
            changedVersion ? 2 : 1,
            'OnJoinedActor request resolves a new version and retains an unchanged route'
          );
          if (!changedVersion) assert.strictEqual(result.route, retained.route);
          assert.equal(await manager.findActor(actorId), undefined);
          assert.equal(handlers, 0);
        }
      },
      serial: new ZLinkSpotSerialTurnExecutor(),
      timers: {},
      handlers: {},
      actorHandlers: new framework.ZLinkSpotActorHandlerRegistryRuntime().addPacket({
        kind: framework.ZLinkActorPacketKind.Send,
        packetName: 'Probe',
        actorType: Player,
        handlerType: Probe
      })
    });
    if (departed) activation.commitActorDeparture(actorId);
    const spotOptions = new ZLinkSpotRuntimeOptionsFactory({
      registration: { spotNodes: new Map() },
      meshRouters: { spotRouterChannelIdByMesh: () => new Map() },
      actorManager: () => manager,
      locationLifecycle: () => undefined,
      createLocationSpotRouteResolver: () => undefined,
      boundSessionRelay: {},
      spotNodeRuntime: () => undefined,
      dispatchErrorReporter: () => undefined
    }).create(undefined);
    const spotManager = new DefaultZLinkSpotManager({
      ...spotOptions,
      spotFactories: [],
      detachedTaskRunner: runner
    });
    spotManager.activations.register(activation);
    const admission = spotManager.formalRemoteActorAdmissions;
    admission.begin({
      transferId: 'move',
      actorId,
      actorType: 'player',
      spotId,
      actorRef: { actorId, nodeRid: 'source', objectGeneration: 7n }
    });
    admission.complete('move', {
      accepted: true,
      actorRef: { actorId, nodeRid: 'target', objectGeneration: 7n }
    });
    admission.beginRestore('move');
    const parts = [
      Message.from(
        Buffer.from(
          framework.encodeStreamHeader({
            kind: framework.ZLinkStreamMessageKind.Send,
            codec: framework.ZLinkStreamCodec.Json,
            flags: framework.ZLinkStreamHeaderFlags.None,
            name: 'Probe',
            metadata: new Map()
          })
        )
      ),
      Message.from(Buffer.from('{}'))
    ];
    const probe = async () => {
      await spotManager.dispatchRoutedActorPacket('remote', spotId, actorId, parts);
      assert.equal(handlers, 0, 'ordinary handler must wait for target openAdmission');
    };
    const park = async () => {
      const payload = Message.from(
        Buffer.from(JSON.stringify({ order: admission.get('move').parked.length + 1 }))
      );
      try {
        await spotManager.dispatchRoutedActorPacket(
          'remote',
          spotId,
          actorId,
          [parts[0], payload],
          false,
          undefined,
          { actorId, nodeRid: 'target', objectGeneration: 7n }
        );
      } finally {
        payload.close();
      }
      assert.equal(handlers, 0);
    };
    const committed = {
      storeVersion: { value: 'v12' },
      objectGeneration: 7n,
      authorityOwnerGeneration: 12n,
      ownerLeaseGeneration: 1n
    };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      invalidateActorRoute: (id) => resolver.invalidateActorRoute(id),
      observeActorAuthorityVersion: (id, version) =>
        resolver.observeActorAuthorityVersion(id, version),
      meshNode: () => node,
      actorManager: () => manager,
      spotManager: () => spotManager,
      locationStore: () => ({
        async readAuthority() {
          await probe();
          return { kind: 'snapshot', ...committed };
        }
      }),
      registration: {
        spotNodes: new Map([
          [
            'play',
            {
              actorFactoryRegistrations: {
                player: { implementation: Player, relocation: { kind: 'recreate' } }
              }
            }
          ]
        ])
      }
    });
    const materialized = await runtime.materializeTargetEnvelope('play', {
      aggregateId: 'move',
      aggregateGeneration: 1n,
      participants: [
        {
          key: actorKey,
          objectKind: 'actor',
          stableType: 'player',
          objectGeneration: 7n,
          authorityOwnerGeneration: 11n,
          applicationState: new Uint8Array(),
          boundSessionState: new Uint8Array(),
          queuedMessages: [],
          timers: []
        }
      ],
      memberships: [{ actorKey, spotKey, membershipEpoch: 3n, spotObjectGeneration: 5n }]
    });
    try {
      assert.equal(await manager.findActor(actorId), undefined);
      assert.equal(manager.getState(actorId).ownerLeaseGeneration, undefined);
      await probe();
      const stage = {
        ...materialized,
        phase: 'ready',
        releaseActivation() {},
        releaseJoinAdmission() {},
        offer: { reservation: {}, prepareFingerprint: 'move' }
      };
      runtime.commitTargetReservation = async () => committed;
      runtime.clearTargetRelocationPublication = async () => {
        await probe();
        return committed;
      };
      runtime.submitActorJoinSourceLeave = async () => {};
      runtime.publishSessionRoutes = async () => {
        await probe();
        await park();
      };
      await park();
      assert.equal(admission.get('move').parked.length, 1);
      await runtime.finalizeTargetStage('play', 'move', stage).catch((error) => {
        throw error.cause ?? error;
      });
      assert.equal(await manager.findActor(actorId), manager.getState(actorId).actor);
      assert.equal(activation.resolveJoinedActor(actorId), manager.getState(actorId).actor);
      assert.equal(admission.get('move').parked.length, 0);
      assert.equal(admission.get('move').migrated, true);
      const payload = Message.from(Buffer.from('{"order":3}'));
      const ordinary = spotManager.dispatchRoutedActorPacket('local', spotId, actorId, [
        parts[0],
        payload
      ]);
      releaseFirst();
      await ordinary;
      payload.close();
      assert.equal(handlers, 3);
      assert.deepEqual(
        order,
        [1, 2, 3],
        'retained arrivals keep their order ahead of ordinary ingress'
      );
    } finally {
      releaseFirst();
      admission.delete('move');
      for (const part of parts) part.close();
      activation.commitActorDeparture(actorId);
      await manager.abortRelocationActor(actorId);
    }
  });
}
