const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const follow = require('../../packages/framework/dist/runtime/actors/actor-message-follow-context');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const stream = require('../../packages/framework/dist/runtime/streams/protocol');

for (const operation of ['join', 'user_spot', 'instance_spot']) {
  test(`${operation} retains the Ready Spot route for a late Actor Message Follow send`, async () => {
    const target = {
      routerChannelId: 'mesh',
      targetNodeRid: 'target',
      spotId: 'matchmaking',
      spotKind:
        operation === 'instance_spot'
          ? framework.ZLinkSpotKind.Instance
          : framework.ZLinkSpotKind.User,
      targetSpotGeneration: 7n,
      targetNodeGeneration: 11n,
      targetOwnerId: 'owner-target',
      authorityOwnerGeneration: 13n,
      ownerLeaseGeneration: 17n,
      authorityStoreVersion: 'spot-ready-version'
    };
    const source = {
      meshName: 'mesh',
      objectGeneration: 19n,
      ownerId: 'owner-source',
      ownerLeaseGeneration: 2n,
      nodeRid: 'source',
      nodeGeneration: 3n,
      authorityOwnerGeneration: 23n
    };
    const committed = {
      objectGeneration: 19n,
      authorityOwnerGeneration: 29n,
      ownerId: 'owner-target',
      ownerLeaseGeneration: 17n,
      allocation: {
        objectKind: 'actor',
        state: 'active',
        stableType: 'Player',
        descriptor: { meshName: 'mesh', rid: 'target' },
        descriptorLifecycleGeneration: 11n
      }
    };
    const submissions = [];
    const node = {
      status: () => ({ routingId: 'source', lifecycleGeneration: 3n }),
      sealSpotMessageFollowIngress: () => ({}),
      abortSpotMessageFollowIngress: () => true,
      commitSpotMessageFollowIngress: async () => true,
      async sendToInstanceSpot(route) {
        submissions.push({
          nodeRid: route.targetNodeRid,
          spotId: route.targetSpotId,
          generation: route.objectGeneration,
          fence: {
            spot: { spotId: route.targetSpotId, generation: route.objectGeneration },
            targetNodeRid: route.targetNodeRid,
            targetNodeGeneration: route.targetNodeGeneration,
            authorityOwnerGeneration: route.authorityOwnerGeneration,
            ownerLeaseGeneration: route.leaseGeneration,
            storeVersion: route.storeVersion
          }
        });
        return 0;
      },
      entrySpot: () => ({
        async sendToSpot(nodeRid, spotId, generation, _parts, options) {
          submissions.push({
            nodeRid: String(nodeRid),
            spotId: String(spotId),
            generation,
            fence: options.routeFence
          });
          return 0;
        }
      })
    };
    const transport = new framework.ZLinkRuntimeRouteTransport(
      () => undefined,
      undefined,
      () => ({ meshNode: () => node })
    );
    const handoff = new framework.ZLinkActorHandoffCoordinator({
      routedTransport: transport,
      requestSource: () => source,
      validateReplySource: () => true
    });
    const actor = { context: { actorId: 'alice' } };
    const state = {
      actorId: 'alice',
      actorType: 'Player',
      actor,
      meshName: 'mesh',
      nativeActorRef: { actorId: 'alice', nodeRid: 'source', generation: 19n }
    };
    const spotAuthority = {
      ...committed,
      objectGeneration: 7n,
      authorityOwnerGeneration: 13n,
      storeVersion: { value: target.authorityStoreVersion },
      allocation: {
        ...committed.allocation,
        objectKind: operation === 'instance_spot' ? 'instance_spot' : 'user_spot',
        stableType: 'Matchmaking'
      }
    };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          [
            'mesh',
            {
              actorFactoryRegistrations: { Player: { relocation: { kind: 'recreate' } } },
              spotFactoryRegistrations: { Matchmaking: { relocation: { kind: 'recreate' } } },
              instanceSpotFactoryRegistrations: {
                Matchmaking: { relocation: { kind: 'recreate' } }
              }
            }
          ]
        ])
      },
      meshNode: () => node,
      spotManager: () => ({
        pendingSpotCloseDecision() {},
        isSpotClosing: () => false,
        async completeRelocationSource() {}
      }),
      actorManager: () => ({ async completeRelocationSource() {} }),
      liveDescriptors: async () => [{ rid: 'target', lifecycleGeneration: 11n }],
      locationStore: () => ({
        async readAuthority(key) {
          const row =
            key.value === encodeAuthorityKey('actor', 'alice').value ? committed : spotAuthority;
          return {
            ...row,
            kind: 'snapshot',
            payload: Buffer.alloc(0),
            authorityOwnerGeneration: source.authorityOwnerGeneration,
            ownerId: source.ownerId,
            ownerLeaseGeneration: source.ownerLeaseGeneration,
            allocation: {
              ...row.allocation,
              descriptor: { meshName: 'mesh', rid: 'source' },
              descriptorLifecycleGeneration: source.nodeGeneration
            }
          };
        }
      }),
      actorTransfer: {
        async prepareMaintenanceSession() {
          handoff.begin('alice', 19n, 'source');
          return {
            handoffBacklog: handoff.snapshot('alice'),
            async commit(route, actorRef, owner) {
              handoff.complete('alice', route, actorRef, [], owner);
            }
          };
        }
      }
    });
    // The wire peer confirms the target-only commit. The complete source Join,
    // capture, commit, Follow and route transport use their production paths.
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.runCoordinator = async (_mesh, _descriptor, _authority, captured) => {
      await captured.commitSource(
        new Map([
          [encodeAuthorityKey('actor', 'alice').value, committed],
          [
            encodeAuthorityKey(
              operation === 'instance_spot' ? 'instance_spot' : 'user_spot',
              'matchmaking'
            ).value,
            spotAuthority
          ]
        ])
      );
    };
    if (operation === 'join') {
      await runtime.relocateActorJoin({
        meshName: 'mesh',
        actor,
        state,
        target,
        relocationId: '11111111111111111111111111111111'
      });
    } else {
      const {
        ZLinkSpotSerialTurnExecutor
      } = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
      await runtime.relocateSpotAggregate(
        'mesh',
        {
          spotId: 'matchmaking',
          spot: {},
          serial: new ZLinkSpotSerialTurnExecutor(),
          async captureRelocation() {
            return { timers: [] };
          },
          async commitRelocation() {
            return true;
          },
          commitActorDeparture() {}
        },
        operation,
        [state],
        { rid: 'target', lifecycleGeneration: 11n }
      );
    }
    const parts = [
      ZLinkBufferMessage.from(
        stream.encodeStreamHeader({
          kind: stream.ZLinkStreamMessageKind.Send,
          codec: stream.ZLinkStreamCodec.Json,
          flags: stream.ZLinkStreamHeaderFlags.None,
          name: 'RoomAssignment',
          metadata: new Map()
        })
      ),
      ZLinkBufferMessage.from(Buffer.from('{"roomId":"room-1"}'))
    ];
    const ref = follow.attachActorMessageFollowContext(
      {
        actorId: 'alice',
        objectGeneration: 19n,
        nodeRid: 'source',
        meshName: 'mesh'
      },
      follow.createInitialActorMessageFollowContext(
        {
          actorRef: { actorId: 'alice', objectGeneration: 19n, nodeRid: 'source' },
          ownerId: source.ownerId,
          ownerLeaseGeneration: source.ownerLeaseGeneration,
          ownerNodeGeneration: source.nodeGeneration,
          authorityOwnerGeneration: source.authorityOwnerGeneration,
          ownerNodeRid: 'source'
        },
        parts,
        false
      )
    );
    try {
      await handoff.capture('remote', 'alice', parts, false, undefined, ref);
      assert.deepEqual(submissions, [
        {
          nodeRid: 'target',
          spotId: 'matchmaking',
          generation: 7n,
          fence: {
            spot: { spotId: 'matchmaking', generation: 7n },
            targetNodeRid: 'target',
            targetNodeGeneration: 11n,
            authorityOwnerGeneration: 13n,
            ownerLeaseGeneration: 17n,
            storeVersion: 'spot-ready-version'
          }
        }
      ]);
    } finally {
      parts.forEach((part) => part.close());
    }
  });
}
