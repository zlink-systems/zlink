import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createHash } from 'node:crypto';
import { RequestResult } from '@zlink-systems/zlink';
import type {
  ZLinkAggregateId,
  ZLinkAuthorityKey,
  ZLinkLocationOwnerToken,
  ZLinkObjectCreationTarget
} from '../../packages/framework/src/contracts/Locations';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkSpotCloseReason,
  ZLinkSpotCreateState
} from '../../packages/framework/src/contracts';
import {
  ZLinkInMemoryAuthorityStore
} from '../../packages/framework/src/runtime/locations/in-memory-authority-store';
import {
  ZLinkInMemoryLocationStore
} from '../../packages/framework/src/runtime/locations/in-memory-location-store';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import {
  ZLinkUserSpotCreationCoordinator
} from '../../packages/framework/src/runtime/host/user-spot-creation-coordinator';
import {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} from '../../packages/framework/src/runtime/framework-errors-internal';
import {
  ZLinkPublicSpotManager
} from '../../packages/framework/src/runtime/spots/spot-manager-public';
import {
  invokeSpotClosing
} from '../../packages/framework/src/runtime/spots/spot-closing';
import {
  encodeServiceUserSpotAuthorityPayload
} from '../../packages/framework/src/runtime/foundation/service-authority-payload-codec';

test('owner lease uses exact claim read renew and release fencing', async () => {
  let now = 100;
  const store = new ZLinkInMemoryLocationStore(() => new Date(now));
  const claimed = await store.claimOwnerLease('owner-a', 50);
  assert.equal(claimed.kind, 'claimed');
  if (claimed.kind !== 'claimed') return;
  assert.equal(claimed.token.leaseGeneration, 1n);
  assert.deepEqual(await store.claimOwnerLease('owner-a', 50), { kind: 'conflict' });
  const found = await store.readOwnerLease('owner-a');
  assert.equal(found.kind, 'found');
  assert.deepEqual(
    await store.renewOwnerLease({ ownerId: 'owner-a', leaseGeneration: 2n }, 50),
    { kind: 'stale' }
  );
  const renewed = await store.renewOwnerLease(claimed.token, 50);
  assert.equal(renewed.kind, 'renewed');
  assert.equal(
    await store.releaseOwnerLease({ ownerId: 'owner-a', leaseGeneration: 2n }),
    'stale'
  );
  assert.equal(await store.releaseOwnerLease(claimed.token), 'released');
  now++;
  const reclaimed = await store.claimOwnerLease('owner-a', 50);
  assert.equal(reclaimed.kind, 'claimed');
  if (reclaimed.kind === 'claimed') {
    assert.ok(reclaimed.token.leaseGeneration > claimed.token.leaseGeneration);
  }
});

test('generic reservation is the only Missing to Pending to Active path', async () => {
  const live = new Set(['mesh:node-a:1:owner-a:1']);
  const store = authority(live);
  const reserved = await store.reserve(reserveRequest('room', target('node-a', 'owner-a')));
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  assert.equal(reserved.creating.allocation.state, 'reserved');
  assert.equal(reserved.creating.allocation.stableType, 'room');
  assert.equal(reserved.creating.ownerId, 'owner-a');
  assert.deepEqual(reserved.creating.pendingCreation, {
    reservationId: reserved.reservationId,
    requestContentReference: 'request:room',
    requestSha256: Buffer.alloc(32, 1),
    requestEncodedSize: 10n
  });

  const committed = await store.commit({
    key: { kind: 'user_spot', globalId: 'room' },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: target('node-a', 'owner-a'),
    readyPayload: Buffer.from('ready')
  });
  assert.equal(committed.kind, 'committed');
  if (committed.kind !== 'committed') return;
  assert.equal(committed.ready.allocation.state, 'active');
  assert.equal(committed.ready.objectGeneration, reserved.creating.objectGeneration);
  assert.equal(committed.ready.authorityOwnerGeneration, reserved.creating.authorityOwnerGeneration);
  assert.equal(committed.ready.pendingCreation, undefined);
});

test('creation abort cleans pending capacity without requiring a live target', async () => {
  const live = new Set(['mesh:node-a:1:owner-a:1']);
  const store = authority(live);
  const reserved = await store.reserve(reserveRequest('ephemeral', target('node-a', 'owner-a')));
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  live.clear();
  assert.deepEqual(await store.abort({
    key: { kind: 'user_spot', globalId: 'ephemeral' },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: target('node-a', 'owner-a')
  }), { kind: 'aborted' });
  assert.equal((await store.readAuthority(authorityKey('ephemeral'))).kind, 'missing');
});

test('Actor creation terminal is scoped to the exact source operation and published atomically', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const placement = target('node-a', 'owner-a');
  const actorRequest = {
    ...reserveRequest('actor-a', placement),
    key: { kind: 'actor' as const, globalId: 'actor-a' },
    capacity: { actors: 1, spots: 0 },
    intent: {
      ...reserveRequest('actor-a', placement).intent,
      stableType: 'player'
    }
  };
  const operation = {
    sourceNodeRid: 'source-node',
    sourceNodeGeneration: 7n,
    operationId: { high: 0n, low: 1n }
  };
  const distinctOperation = {
    ...operation,
    operationId: { high: 0n, low: 2n }
  };
  const envelope = Buffer.from('creation-operation-terminal-v1:created');
  const reserved = await store.reserve(actorRequest);
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  await assert.rejects(() => store.commit({
    key: actorRequest.key,
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: placement,
    readyPayload: Buffer.from('ready')
  }), /completeCreation/);
  const pending = await store.readAuthority(encodeAuthorityKey('actor', 'actor-a'));
  assert.equal(pending.kind, 'snapshot');
  if (pending.kind === 'snapshot') assert.equal(pending.allocation.state, 'reserved');
  const committed = await store.completeCreation({
    key: actorRequest.key,
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: placement,
    completion: {
      kind: 'created',
      readyPayload: Buffer.from('ready'),
      terminal: {
        operation,
        terminalEnvelope: envelope,
        terminalEnvelopeSha256: createHash('sha256').update(envelope).digest(),
        operationDeadline: new Date(1_000)
      }
    }
  });
  assert.equal(committed.kind, 'created');
  const found = await store.readCreationTerminal(operation);
  assert.equal(found.kind, 'found');
  if (found.kind === 'found') assert.equal(found.record.state, 'created');
  assert.equal((await store.readCreationTerminal(distinctOperation)).kind, 'missing');
});

test('Actor rejection removes Creating authority and does not leak its reply to another operation', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const placement = target('node-a', 'owner-a');
  const actorRequest = {
    ...reserveRequest('actor-rejected', placement),
    key: { kind: 'actor' as const, globalId: 'actor-rejected' },
    capacity: { actors: 1, spots: 0 },
    intent: {
      ...reserveRequest('actor-rejected', placement).intent,
      stableType: 'player'
    }
  };
  const operation = {
    sourceNodeRid: 'source-node',
    sourceNodeGeneration: 7n,
    operationId: { high: 0n, low: 3n }
  };
  const envelope = Buffer.from('creation-operation-terminal-v1:rejected');
  const reserved = await store.reserve(actorRequest);
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  const rejected = await store.completeCreation({
    key: actorRequest.key,
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: placement,
    completion: {
      kind: 'rejected',
      terminal: {
        operation,
        terminalEnvelope: envelope,
        terminalEnvelopeSha256: createHash('sha256').update(envelope).digest(),
        operationDeadline: new Date(1_000)
      }
    }
  });
  assert.equal(rejected.kind, 'rejected');
  assert.equal((await store.readAuthority(
    encodeAuthorityKey('actor', 'actor-rejected')
  )).kind, 'missing');
  const terminal = await store.readCreationTerminal(operation);
  assert.equal(terminal.kind, 'found');
  if (terminal.kind === 'found') assert.equal(terminal.record.state, 'rejected');
  assert.equal((await store.readCreationTerminal({
    ...operation,
    operationId: { high: 0n, low: 4n }
  })).kind, 'missing');
});

test('public User Spot coordinator hides Pending, runs one factory, then publishes Ready generation', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const publicationOrder: string[] = [];
  let publishedMesh: string | undefined;
  let publishedRoute: unknown;
  let forgottenMesh: string | undefined;
  let forgottenRoute: unknown;
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    publishReadyRoute: (meshName, route) => {
      publicationOrder.push('route');
      publishedMesh = meshName;
      publishedRoute = route;
    },
    forgetReadyRoute: (meshName, route) => {
      publicationOrder.push('forget');
      forgottenMesh = meshName;
      forgottenRoute = route;
    },
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    }),
    pollIntervalMs: 1
  });
  let release!: () => void;
  const initialize = new Promise<void>(resolve => {
    release = resolve;
  });
  let materializations = 0;
  const request = {
    meshName: 'mesh',
    spotId: 'room-42',
    stableType: 'room',
    requestPayload: Buffer.from('create-room'),
    timeoutMs: 1_000
  };
  const first = coordinator.getOrCreate(request, async () => {
    materializations++;
    await initialize;
    return {
      spotId: request.spotId,
      state: ZLinkSpotCreateState.Created,
      publication: {
        publish: () => publicationOrder.push('publish'),
        abort: () => publicationOrder.push('abort')
      }
    };
  });
  await new Promise(resolve => setImmediate(resolve));
  const pending = await store.readAuthority(authorityKey('room-42'));
  assert.equal(pending.kind, 'snapshot');
  if (pending.kind === 'snapshot') {
    assert.equal(pending.allocation.state, 'reserved');
  }

  const joined = coordinator.getOrCreate(request, async () => {
    materializations++;
    throw new Error('CAS loser must not materialize.');
  });
  release();
  const [created, existing] = await Promise.all([first, joined]);
  assert.equal(materializations, 1);
  assert.equal(created.result.state, ZLinkSpotCreateState.Created);
  assert.equal(existing.result.state, ZLinkSpotCreateState.Existing);
  assert.equal(created.spot.objectGeneration, existing.spot.objectGeneration);
  assert.ok(created.spot.objectGeneration > 0n);
  const ready = await store.readAuthority(authorityKey('room-42'));
  assert.equal(ready.kind, 'snapshot');
  if (ready.kind === 'snapshot') {
    assert.equal(ready.allocation.state, 'active');
    assert.equal(ready.objectGeneration, created.spot.objectGeneration);
    assert.equal(publishedMesh, 'mesh');
    assert.deepEqual(publishedRoute, {
      spot: { spotId: 'room-42', generation: created.spot.objectGeneration },
      targetNodeRid: 'node-a',
      targetNodeGeneration: 1n,
      authorityOwnerGeneration: ready.authorityOwnerGeneration,
      ownerLeaseGeneration: ready.ownerLeaseGeneration,
      storeVersion: ready.storeVersion.value
    });
  }
  assert.deepEqual(publicationOrder, ['route', 'publish']);
  assert.equal(await coordinator.close(created.spot, async () => true), true);
  assert.equal(forgottenMesh, 'mesh');
  assert.deepEqual(forgottenRoute, publishedRoute);
  assert.deepEqual(publicationOrder, ['route', 'publish', 'forget']);
});

test('User Spot rejection is terminal and aborts the Location reservation without Relocation Store', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  const result = await coordinator.getOrCreate({
    meshName: 'mesh',
    spotId: 'rejected-room',
    stableType: 'room',
    requestPayload: Buffer.from('reject-me'),
    timeoutMs: 1_000
  }, async () => ({
    spotId: 'rejected-room',
    state: ZLinkSpotCreateState.Rejected,
    reply: { reason: 'closed' }
  }));
  assert.equal(result.result.state, ZLinkSpotCreateState.Rejected);
  assert.deepEqual(result.result.reply, { reason: 'closed' });
  assert.equal(
    (await store.readAuthority(authorityKey('rejected-room'))).kind,
    'missing'
  );
});

test('User Spot creation applies one deadline signal through factory and abort cleanup', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  await assert.rejects(
    () => coordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'deadline-room',
      stableType: 'room',
      requestPayload: Buffer.from('slow'),
      timeoutMs: 5
    }, async (_target, _authority, signal) => await new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason), { once: true });
    })),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
  );
  assert.equal(
    (await store.readAuthority(authorityKey('deadline-room'))).kind,
    'missing'
  );
});

test('User Spot reconciliation and abort cleanup stop at their bounded cleanup deadline', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  store.abort = async () => await new Promise(() => undefined);
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    }),
    cleanupTimeoutMs: 5
  });
  const started = Date.now();
  await assert.rejects(
    () => coordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'cleanup-deadline-room',
      stableType: 'room',
      requestPayload: Buffer.from('create'),
      timeoutMs: 1_000
    }, async () => {
      throw new Error('factory failed');
    }),
    AggregateError
  );
  assert.ok(Date.now() - started < 100);
});

test('User Spot reservation maps capacity exhaustion and Pending expiry to exact typed errors', async () => {
  const base = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const exhausted = Object.create(base) as ZLinkInMemoryAuthorityStore;
  exhausted.reserve = async () => ({ kind: 'placementCapacityExhausted' });
  const targetOptions = {
    meshName: 'mesh',
    nodeRid: 'node-a',
    nodeGeneration: 1n,
    owner: owner('owner-a', 1n),
    isLocal: true
  } as const;
  const exhaustedCoordinator = new ZLinkUserSpotCreationCoordinator({
    store: exhausted,
    target: async () => targetOptions
  });
  await assert.rejects(
    () => exhaustedCoordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'full-room',
      stableType: 'room',
      requestPayload: Buffer.from('create'),
      timeoutMs: 100
    }, async () => {
      throw new Error('factory must not start');
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.CapacityExceeded
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted
  );

  const pendingStore = authority(new Set(['mesh:node-a:1:owner-a:1']));
  await pendingStore.reserve(reserveRequest('pending-room', target('node-a', 'owner-a')));
  const pendingCoordinator = new ZLinkUserSpotCreationCoordinator({
    store: pendingStore,
    target: async () => targetOptions,
    pollIntervalMs: 1
  });
  await assert.rejects(
    () => pendingCoordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'pending-room',
      stableType: 'room',
      requestPayload: Buffer.from('same-request'),
      timeoutMs: 5
    }, async () => {
      throw new Error('CAS loser must not start factory');
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
  );
});

test('User Spot placement excludes a capacity-race loser and reserves the next candidate', async () => {
  const base = authority(new Set([
    'mesh:node-a:1:owner-a:1',
    'mesh:node-b:1:owner-b:1'
  ]));
  const store = Object.create(base) as ZLinkInMemoryAuthorityStore;
  const reserve = base.reserve.bind(base);
  let reserveCalls = 0;
  store.reserve = async (request, signal) => {
    reserveCalls++;
    return reserveCalls === 1
      ? { kind: 'placementCapacityExhausted' }
      : reserve(request, signal);
  };
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async (_request, _signal, excluded) => {
      const node = excluded?.has('node-a') === true ? 'node-b' : 'node-a';
      return {
        meshName: 'mesh',
        nodeRid: node,
        nodeGeneration: 1n,
        owner: owner(node === 'node-a' ? 'owner-a' : 'owner-b', 1n),
        isLocal: true
      };
    }
  });

  const result = await coordinator.getOrCreate({
    meshName: 'mesh',
    spotId: 'capacity-race-room',
    stableType: 'room',
    requestPayload: Buffer.from('create'),
    timeoutMs: 1_000
  }, async target => ({
    spotId: 'capacity-race-room',
    state: ZLinkSpotCreateState.Created,
    target
  }));

  assert.equal(reserveCalls, 2);
  assert.equal(result.spot.nodeRid, 'node-b');
});

test('User Spot production placement maps no target to retriable capacity exhaustion and provider faults to RequestFailed', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const request = {
    meshName: 'mesh',
    spotId: 'placement-room',
    stableType: 'room',
    requestPayload: Buffer.from('create'),
    timeoutMs: 100
  };
  const noTarget = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => undefined
  });
  await assert.rejects(
    () => noTarget.getOrCreate(request, async () => {
      throw new Error('factory must not start');
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.CapacityExceeded
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted
  );

  const providerFault = new Error('provider unavailable');
  const failedProvider = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => {
      throw providerFault;
    }
  });
  await assert.rejects(
    () => failedProvider.getOrCreate(request, async () => {
      throw new Error('factory must not start');
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InternalFailure
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestFailed
      && error.cause === providerFault
  );

  const storeFault = new Error('store unavailable');
  const failedStore = authority(new Set(['mesh:node-a:1:owner-a:1']));
  failedStore.reserve = async () => {
    throw storeFault;
  };
  const failedReservation = new ZLinkUserSpotCreationCoordinator({
    store: failedStore,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  await assert.rejects(
    () => failedReservation.getOrCreate(request, async () => {
      throw new Error('factory must not start');
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InternalFailure
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestFailed
      && error.cause === storeFault
  );
});

test('User Spot creation reserves at the source and materializes exact Pending content at the remote target', async () => {
  const store = authority(new Set(['mesh:node-b:2:owner-b:2']));
  let materializations = 0;
  let published = false;
  let release!: () => void;
  const initialized = new Promise<void>(resolve => {
    release = resolve;
  });
  const targetCoordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => undefined
  });
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-b',
      nodeGeneration: 2n,
      owner: owner('owner-b', 2n),
      isLocal: false
    }),
    remoteCreate: async (_meshName, _targetNodeRid, record) => {
      const coordinated = await targetCoordinator.handleRemoteCreate(
        { kind: 'userSpotCreate', correlation: 1n, operation: { high: 1n, low: 1n }, ...record },
        async requestPayload => {
          materializations++;
          assert.deepEqual(requestPayload, Buffer.from('create'));
          await initialized;
          return {
            spotId: record.spotId,
            state: ZLinkSpotCreateState.Created,
            publication: {
              publish: () => { published = true; },
              abort: () => { published = false; }
            }
          };
        }
      );
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: {
          kind: 'userSpotCreate',
          createResult: 'created',
          spotId: String(coordinated.spot.spotId),
          objectGeneration: coordinated.spot.objectGeneration
        }
      };
    }
  });
  const create = () => coordinator.getOrCreate({
    meshName: 'mesh',
    spotId: 'remote-selected-room',
    stableType: 'room',
    requestPayload: Buffer.from('create'),
    timeoutMs: 100
  }, async () => {
      throw new Error('remote owner must not start local factory');
  });
  const first = create();
  await new Promise(resolve => setImmediate(resolve));
  const second = create();
  assert.equal(published, false);
  release();
  const [created, joined] = await Promise.all([first, second]);
  assert.equal(materializations, 1);
  assert.equal(created.result.state, ZLinkSpotCreateState.Created);
  assert.equal(joined.spot.objectGeneration, created.spot.objectGeneration);
  assert.equal(published, true);
  const ready = await store.readAuthority(authorityKey('remote-selected-room'));
  assert.equal(ready.kind, 'snapshot');
  if (ready.kind === 'snapshot') assert.equal(ready.allocation.state, 'active');
});

test('remote User Spot target aborts the exact reservation when materialization fails', async () => {
  const store = authority(new Set(['mesh:node-b:2:owner-b:2']));
  const targetCoordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => undefined
  });
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-b',
      nodeGeneration: 2n,
      owner: owner('owner-b', 2n),
      isLocal: false
    }),
    remoteCreate: async (_meshName, _targetNodeRid, record) => {
      await assert.rejects(
        () => targetCoordinator.handleRemoteCreate(
          { kind: 'userSpotCreate', correlation: 1n, operation: { high: 1n, low: 1n }, ...record },
          async () => {
            throw new Error('factory failed');
          }
        ),
        /factory failed/
      );
      assert.equal(
        (await store.readAuthority(authorityKey(record.spotId))).kind,
        'missing'
      );
      throw new Error('remote target rejected creation');
    }
  });
  await assert.rejects(
    () => coordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'remote-failed-room',
      stableType: 'room',
      requestPayload: Buffer.from('create'),
      timeoutMs: 1_000
    }, async () => {
      throw new Error('remote owner must not start local factory');
    }),
    /remote target rejected creation/
  );
});

test('User Spot Ready commit Store rejection is exposed as RequestFailed with the original cause', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const commitFault = new Error('commit unavailable');
  store.commit = async () => {
    throw commitFault;
  };
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  await assert.rejects(
    () => coordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'commit-failed-room',
      stableType: 'room',
      requestPayload: Buffer.from('create'),
      timeoutMs: 100
    }, async () => ({
      spotId: 'commit-failed-room',
      state: ZLinkSpotCreateState.Created
    })),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InternalFailure
      && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestFailed
      && error.cause === commitFault
  );
  assert.equal(
    (await store.readAuthority(authorityKey('commit-failed-room'))).kind,
    'missing'
  );
});

test('User Spot get-or-create returns an existing Ready remote incarnation without local factory work', async () => {
  const store = authority(new Set([
    'mesh:node-a:1:owner-a:1',
    'mesh:node-b:2:owner-b:2'
  ]));
  await createActive(store, 'remote-room', target('node-b', 'owner-b'));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    }),
    pollIntervalMs: 1
  });
  const existing = await coordinator.getOrCreate({
      meshName: 'mesh',
      spotId: 'remote-room',
      stableType: 'room',
      requestPayload: Buffer.from('create'),
      timeoutMs: 1_000
    }, async () => {
      throw new Error('remote CAS loser must not start factory');
    });
  assert.equal(existing.result.state, ZLinkSpotCreateState.Existing);
  assert.equal(existing.spot.nodeRid, 'node-b');
});

test('User Spot close fences the exact generation and deletes authority only after owner close', async () => {
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const closeMutations: string[] = [];
  const compareExchange = store.compareExchangeAuthority.bind(store);
  store.compareExchangeAuthority = async (key, version, mutation, signal) => {
    closeMutations.push(mutation.kind);
    return await compareExchange(key, version, mutation, signal);
  };
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  const created = await coordinator.getOrCreate({
    meshName: 'mesh',
    spotId: 'close-room',
    stableType: 'room',
    requestPayload: Buffer.from('create'),
    timeoutMs: 1_000
  }, async () => ({
    spotId: 'close-room',
    state: ZLinkSpotCreateState.Created
  }));
  let ownerClosed = false;
  assert.equal(await coordinator.close(created.spot, async () => {
    ownerClosed = true;
    return true;
  }), true);
  assert.equal(ownerClosed, true);
  assert.deepEqual(closeMutations, ['put', 'delete']);
  assert.equal(
    (await store.readAuthority(authorityKey('close-room'))).kind,
    'missing'
  );
});

test('Spot closing callback receives exact reason, absolute deadline, and cleanup signal', async () => {
  let observed = false;
  await invokeSpotClosing(async (context, cleanupSignal) => {
    observed = true;
    assert.equal(context.reason, ZLinkSpotCloseReason.HostShutdown);
    assert.ok(context.deadline.getTime() > Date.now());
    assert.equal(cleanupSignal.aborted, false);
  }, ZLinkSpotCloseReason.HostShutdown, 100);
  assert.equal(observed, true);
});

test('Spot closing deadline aborts cleanup and stops waiting for a stuck callback', async () => {
  const started = Date.now();
  let aborted = false;
  await invokeSpotClosing(async (_context, cleanupSignal) => {
    cleanupSignal.addEventListener('abort', () => {
      aborted = true;
    }, { once: true });
    await new Promise<void>(() => undefined);
  }, ZLinkSpotCloseReason.ExplicitClose, 5);
  assert.equal(aborted, true);
  assert.ok(Date.now() - started < 100);
});

test('exact User Spot manager call is single-use and returns the committed SpotRef', async () => {
  class RoomSpot {}
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  let created = 0;
  let staged = false;
  let published = false;
  const manager = new ZLinkPublicSpotManager({
    local: {
      async getOrCreateWithAuthority(
        _mesh: string,
        _type: typeof RoomSpot,
        spotId: string
      ) {
        assert.equal(staged, true);
        assert.equal(published, false);
        created++;
        return { spotId, state: ZLinkSpotCreateState.Created };
      },
      beginUserSpotPublication() {
        staged = true;
      },
      publishUserSpot() {
        published = true;
        staged = false;
      },
      abortUserSpotPublication() {
        staged = false;
      },
      async close() {
        return true;
      }
    } as never,
    coordinator,
    factories: new Map([[
      'mesh',
      new Map([[
        'room',
        {
          implementation: RoomSpot,
          relocation: { kind: 'disabled' } as never
        }
      ]])
    ]]) as never,
    resolver: () => undefined,
    isLocalNode: () => true,
    defaultTimeoutMs: 1_000
  });
  const call = manager.getOrCreate('room-7', 'room')
    .inMesh('mesh')
    .request({ title: 'room' })
    .timeout(500);
  const result = await call.submit();
  assert.equal(created, 1);
  assert.equal(result.state, ZLinkSpotCreateState.Created);
  assert.equal(result.spot.spotId, 'room-7');
  assert.equal(staged, false);
  assert.equal(published, true);
  assert.ok(result.spot.objectGeneration > 0n);
  await assert.rejects(() => call.submit(), /already been submitted/);
});

test('User Spot create reports its first generated SpotId collision without another UUID or reservation', async () => {
  class RoomSpot {}
  const store = authority(new Set(['mesh:node-a:1:owner-a:1']));
  const collision = await store.reserve({
    ...reserveRequest('spot-collision', target('node-a', 'owner-a')),
    intent: {
      ...reserveRequest('spot-collision', target('node-a', 'owner-a')).intent,
      stableType: 'another-room'
    }
  });
  assert.equal(collision.kind, 'reserved');
  if (collision.kind !== 'reserved') return;
  await store.commit({
    key: { kind: 'user_spot', globalId: 'spot-collision' },
    reservationId: collision.reservationId,
    expectedStoreVersion: collision.creating.storeVersion.value,
    target: target('node-a', 'owner-a'),
    readyPayload: Buffer.from('ready')
  });
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  let generated = 0;
  const materialized: string[] = [];
  const manager = new ZLinkPublicSpotManager({
    local: {
      async getOrCreate(_mesh: string, _type: typeof RoomSpot, spotId: string) {
        materialized.push(spotId);
        return { spotId, state: ZLinkSpotCreateState.Created };
      },
      async close() {
        return true;
      }
    } as never,
    coordinator,
    factories: new Map([[
      'mesh',
      new Map([[
        'room',
        { implementation: RoomSpot, relocation: { kind: 'disabled' } as never }
      ]])
    ]]) as never,
    resolver: () => undefined,
    isLocalNode: () => true,
    defaultTimeoutMs: 1_000,
    ridFactory: () => {
      generated++;
      return generated === 1 ? 'spot-collision' : 'spot-unexpected';
    }
  });
  await assert.rejects(
    () => manager.create('room').inMesh('mesh').submit(),
    (error: unknown) =>
      error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.AlreadyExists
  );
  assert.equal(generated, 1);
  assert.deepEqual(materialized, []);
});

test('User Spot create rejects its first remote SpotId collision while getOrCreate returns that Ready incarnation', async () => {
  class RoomSpot {}
  const store = authority(new Set([
    'mesh:node-a:1:owner-a:1',
    'mesh:node-b:2:owner-b:2'
  ]));
  await createActive(store, 'spot-remote-collision', target('node-b', 'owner-b'));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node-a',
      nodeGeneration: 1n,
      owner: owner('owner-a', 1n),
      isLocal: true
    })
  });
  let generated = 0;
  const materialized: string[] = [];
  const manager = new ZLinkPublicSpotManager({
    local: {
      async getOrCreate(_mesh: string, _type: typeof RoomSpot, spotId: string) {
        materialized.push(spotId);
        return { spotId, state: ZLinkSpotCreateState.Created };
      },
      async close() {
        return true;
      }
    } as never,
    coordinator,
    factories: new Map([[
      'mesh',
      new Map([[
        'room',
        { implementation: RoomSpot, relocation: { kind: 'disabled' } as never }
      ]])
    ]]) as never,
    resolver: () => undefined,
    isLocalNode: () => true,
    defaultTimeoutMs: 1_000,
    ridFactory: () => {
      generated++;
      return generated === 1 ? 'spot-remote-collision' : 'spot-unexpected';
    }
  });

  await assert.rejects(
    () => manager.create('room').inMesh('mesh').submit(),
    (error: unknown) =>
      error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.AlreadyExists
  );
  assert.equal(generated, 1);
  assert.deepEqual(materialized, []);

  const existing = await manager.getOrCreate('spot-remote-collision', 'room')
    .inMesh('mesh')
    .submit();
  assert.equal(existing.state, ZLinkSpotCreateState.Existing);
  assert.equal(existing.spot.nodeRid, 'node-b');
});

test('User Spot manager close sends the exact authority fence to the remote owner', async () => {
  const current = {
    spotId: 'remote-close',
    objectGeneration: 1n,
    meshName: 'mesh',
    nodeRid: 'node-b'
  } as const;
  let localCloseCalled = false;
  let remoteCloseCalled = false;
  let forgottenSpot: unknown;
  let forgottenSnapshot: unknown;
  const snapshot = {
    allocation: { descriptorLifecycleGeneration: 2n },
    authorityOwnerGeneration: 3n,
    storeVersion: { value: 'version-1' }
  };
  const manager = new ZLinkPublicSpotManager({
    local: {
      async close() {
        localCloseCalled = true;
        return true;
      }
    } as never,
    coordinator: {
      async resolveCloseTarget() {
        return { spot: current, snapshot };
      }
    } as never,
    factories: new Map(),
    resolver: () => undefined,
    isLocalNode: () => false,
    defaultTimeoutMs: 1_000,
    forgetReadyRoute: (spot, routeSnapshot) => {
      forgottenSpot = spot;
      forgottenSnapshot = routeSnapshot;
    },
    remoteClose: async (_meshName, targetNodeRid, request) => {
      remoteCloseCalled = true;
      assert.equal(targetNodeRid, 'node-b');
      assert.deepEqual(request.target, {
        spotId: 'remote-close',
        objectGeneration: 1n,
        targetNodeRid: 'node-b',
        targetNodeGeneration: 2n,
        authorityOwnerGeneration: 3n,
        expectedStoreVersion: 'version-1'
      });
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: { kind: 'userSpotClose', closed: true }
      };
    }
  });
  assert.equal(await manager.close(current), true);
  assert.equal(localCloseCalled, false);
  assert.equal(remoteCloseCalled, true);
  assert.deepEqual(forgottenSpot, current);
  assert.deepEqual(forgottenSnapshot, snapshot);
});

test('remote User Spot close remains deleted when the terminal reply is lost', async () => {
  const store = authority(new Set(['mesh:node-b:2:owner-b:2']));
  const ready = await createActive(store, 'reply-loss-close', target('node-b', 'owner-b'));
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => undefined
  });
  const spot = {
    spotId: 'reply-loss-close',
    objectGeneration: ready.objectGeneration,
    meshName: 'mesh',
    nodeRid: 'node-b'
  } as const;
  const manager = new ZLinkPublicSpotManager({
    local: {} as never,
    coordinator,
    factories: new Map(),
    resolver: () => undefined,
    isLocalNode: () => false,
    defaultTimeoutMs: 1_000,
    remoteClose: async (_meshName, _targetNodeRid, request) => {
      await coordinator.handleRemoteClose({
        ...request,
        kind: 'userSpotClose',
        correlation: 1n,
        operation: { high: 1n, low: 1n }
      }, async () => true);
      throw new Error('terminal reply lost');
    }
  });

  await assert.rejects(() => manager.close(spot), /terminal reply lost/);
  assert.equal((await store.readAuthority(authorityKey('reply-loss-close'))).kind, 'missing');
  assert.equal(await manager.close(spot), false);
});

test('relocation matches durable source allocation and only requires the target to be live', async () => {
  const live = new Set([
    'mesh:node-a:1:owner-a:1',
    'mesh:node-b:2:owner-b:2'
  ]);
  const store = authority(live);
  const current = await createActive(store, 'relocate', target('node-a', 'owner-a'));
  live.delete('mesh:node-a:1:owner-a:1');

  const reservation = await store.reserveRelocationCapacity({
    reservationId: '11111111-1111-4111-8111-111111111111',
    authorityKey: authorityKey('relocate'),
    expectedStoreVersion: current.storeVersion,
    objectKind: 'user_spot',
    stableType: 'room',
    sourceDescriptor: { meshName: 'mesh', rid: 'node-a' },
    sourceNodeLifecycleGeneration: 1n,
    sourceOwner: owner('owner-a', 1n),
    targetDescriptor: { meshName: 'mesh', rid: 'node-b' },
    targetNodeLifecycleGeneration: 2n,
    targetOwner: owner('owner-b', 2n),
    capacity: userSpotCapacity('room')
  });
  assert.equal(reservation.kind, 'reserved');
  if (reservation.kind !== 'reserved') return;
  const moved = await store.compareExchangeAuthority(
    authorityKey('relocate'),
    current.storeVersion,
    {
      kind: 'put',
      payload: Buffer.from('moved'),
      generationTransition: 'newOwner',
      targetOwner: owner('owner-b', 2n),
      relocationCapacityFence: reservation.fence
    }
  );
  assert.equal(moved.kind, 'stored');
  if (moved.kind !== 'stored') return;
  assert.equal(moved.ownerId, 'owner-b');
  assert.equal(moved.allocation.descriptor.rid, 'node-b');
  assert.equal(moved.objectGeneration, current.objectGeneration);
  assert.ok(moved.authorityOwnerGeneration > current.authorityOwnerGeneration);
});

test('aggregate prepare reserves one typed bundle until aggregate commit or abort', async () => {
  const live = new Set([
    'mesh:node-a:1:owner-a:1',
    'mesh:node-b:2:owner-b:2'
  ]);
  const store = authority(live);
  const first = await createActive(store, 'aggregate-a', target('node-a', 'owner-a'));
  const second = await createActive(store, 'aggregate-b', target('node-a', 'owner-a'));
  const aggregateId = { value: '33333333-3333-4333-8333-333333333333' } as ZLinkAggregateId;
  const capacity = {
    actors: 0,
    spots: 2,
    spotType: {
      objectKind: 'user_spot' as const,
      stableType: 'room',
      count: 2
    }
  };
  const capacityReservation = await store.reserveRelocationCapacity({
    reservationId: aggregateId.value,
    authorityKey: authorityKey('aggregate-a'),
    expectedStoreVersion: first.storeVersion,
    objectKind: 'user_spot',
    stableType: 'room',
    sourceDescriptor: { meshName: 'mesh', rid: 'node-a' },
    sourceNodeLifecycleGeneration: 1n,
    sourceOwner: owner('owner-a', 1n),
    targetDescriptor: { meshName: 'mesh', rid: 'node-b' },
    targetNodeLifecycleGeneration: 2n,
    targetOwner: owner('owner-b', 2n),
    capacity
  });
  assert.equal(capacityReservation.kind, 'reserved');
  const prepared = await store.prepareAggregate({
    aggregateId,
    aggregateGeneration: 1n,
    participants: [
      {
        authorityKey: authorityKey('aggregate-a'),
        expectedStoreVersion: first.storeVersion,
        ownerTransition: 'newOwner',
        authorityPayload: Buffer.from('aggregate-ready-a'),
        membershipMutation: Buffer.from('membership-a')
      },
      {
        authorityKey: authorityKey('aggregate-b'),
        expectedStoreVersion: second.storeVersion,
        ownerTransition: 'newOwner',
        authorityPayload: Buffer.from('aggregate-ready-b'),
        membershipMutation: Buffer.from('membership-b')
      }
    ],
    inventoryDigest: Buffer.alloc(32, 7),
    targetDescriptor: { meshName: 'mesh', rid: 'node-b' },
    targetDescriptorLifecycleGeneration: 2n,
    capacity,
    targetOwner: owner('owner-b', 2n)
  });
  assert.equal(prepared.kind, 'prepared');
  if (prepared.kind !== 'prepared') return;
  assert.deepEqual(await store.commitAggregate(prepared.fence), { kind: 'committed' });
});

function authority(live: Set<string>): ZLinkInMemoryAuthorityStore {
  return new ZLinkInMemoryAuthorityStore({
    isTargetLive(descriptor, lifecycle, token) {
      return live.has(
        `${descriptor.meshName}:${descriptor.rid}:${lifecycle}:${token.ownerId}:${token.leaseGeneration}`
      );
    }
  }, () => new Date(100));
}

function target(rid: string, ownerId: string): ZLinkObjectCreationTarget {
  const generation = rid === 'node-a' ? 1n : 2n;
  return {
    meshName: 'mesh',
    nodeRid: rid,
    nodeLifecycleGeneration: generation,
    owner: owner(ownerId, generation)
  };
}

function reserveRequest(globalId: string, placement: ZLinkObjectCreationTarget) {
  return {
    key: { kind: 'user_spot' as const, globalId },
    intent: {
      stableType: 'room',
      requestContentReference: `request:${globalId}`,
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 10n
    },
    target: placement,
    creatingPayload: Buffer.from('creating'),
    capacity: userSpotCapacity('room')
  };
}

function userSpotCapacity(stableType: string) {
  return {
    actors: 0,
    spots: 1,
    spotType: {
      objectKind: 'user_spot' as const,
      stableType,
      count: 1
    }
  };
}

async function createActive(
  store: ZLinkInMemoryAuthorityStore,
  globalId: string,
  placement: ZLinkObjectCreationTarget
) {
  const reserved = await store.reserve(reserveRequest(globalId, placement));
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('reservation failed');
  const committed = await store.commit({
    key: { kind: 'user_spot', globalId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: placement,
    readyPayload: encodeServiceUserSpotAuthorityPayload({
      state: 'ready',
      stableType: 'room',
      spotId: globalId,
      ownerId: placement.owner.ownerId,
      ownerLeaseGeneration: placement.owner.leaseGeneration,
      ownerMeshName: placement.meshName,
      ownerNodeRid: String(placement.nodeRid),
      ownerNodeGeneration: placement.nodeLifecycleGeneration
    })
  });
  assert.equal(committed.kind, 'committed');
  if (committed.kind !== 'committed') throw new Error('commit failed');
  return committed.ready;
}

function authorityKey(value: string): ZLinkAuthorityKey {
  return encodeAuthorityKey('user_spot', value);
}

function owner(ownerId: string, generation: bigint): ZLinkLocationOwnerToken {
  return { ownerId, leaseGeneration: generation };
}
