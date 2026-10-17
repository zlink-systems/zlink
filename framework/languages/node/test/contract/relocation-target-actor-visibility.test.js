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
const { Message } = require('@zlink-systems/zlink');

for (const departed of [false, true]) {
  test(`relocation membership shares target admission visibility (departed=${departed})`, async () => {
    const actorId = 'hidden-player';
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
    class Probe {
      handle(_spot, actor) {
        const state = manager.getState(actor.context.actorId);
        // This is the committed source-fence input used by the next Deferred Join.
        assert.equal(state.ownerLeaseGeneration, 1n, 'handler must have a committed owner lease');
        assert.equal(state.locationGeneration, 12n);
        handlers++;
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
      spot: { context: { meshName: 'play' } },
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
      dispatchErrorReporter: () => undefined
    }).create(undefined);
    const coordinator = new framework.ZLinkSpotActorAdmissionCoordinator(spotOptions);
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
      await coordinator.dispatchActorPacket('local', activation, actorId, parts);
      assert.equal(handlers, 0, 'ordinary handler must wait for target openAdmission');
    };
    const committed = {
      objectGeneration: 7n,
      authorityOwnerGeneration: 12n,
      ownerLeaseGeneration: 1n
    };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      meshNode: () => node,
      actorManager: () => manager,
      spotManager: () => ({ resolveRelocationActivation: () => activation }),
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
      runtime.finalizeActorJoinProfiles = async () => {
        await probe();
      };
      runtime.publishSessionRoutes = async () => {
        await probe();
      };
      await runtime.finalizeTargetStage('play', 'move', stage);
      assert.equal(await manager.findActor(actorId), manager.getState(actorId).actor);
      assert.equal(activation.resolveJoinedActor(actorId), manager.getState(actorId).actor);
      await coordinator.dispatchActorPacket('local', activation, actorId, parts);
      assert.equal(handlers, 1);
    } finally {
      for (const part of parts) part.close();
      activation.commitActorDeparture(actorId);
      await manager.abortRelocationActor(actorId);
    }
  });
}
