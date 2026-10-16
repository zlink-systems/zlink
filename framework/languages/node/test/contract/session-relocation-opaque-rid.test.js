const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { RoutingId } = require('@zlink-systems/zlink');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { encodeRoutingIdStorageHex } = require('../../packages/framework/dist/runtime/routing-id');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  OperationKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

async function relocationHarness(ids, nativeBinding = false, hooks = {}) {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  let backend;
  let sessionService;
  let stateful;
  let bindWireSession;
  const completions = new ZLinkMeshCompletionTable();
  if (nativeBinding) {
    stateful = new ServiceStatefulRuntime(
      {
        observePeerRoute: () => ({ generation: 2n, ready: true }),
        observePeerRouteChanged: () => () => {},
        observePeerConnectionIntentRemoved: () => () => {},
        setServiceIngress() {},
        async requestService(_target, parts) {
          const command = wire.decodeStatefulHeader(parts[0]);
          if (command.kind === 'boundSessionBind') {
            bindWireSession = command.sessionRid;
            return [
              wire.encodeStatefulReply(
                command.correlation,
                RequestResult.Ok,
                0,
                command.binding.state === 'active'
                  ? { kind: 'streamBind', bindingGeneration: 6n, authorityOwnerGeneration: 11n }
                  : undefined
              )
            ];
          }
          throw new Error('unexpected Session binding command');
        }
      },
      ids.owner,
      4n
    );
    stateful.rememberActorRoute({
      actor: { nodeRid: ids.source, actorId: 'opaque-relocation-actor', generation: 5n },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n
    });
    backend = new ZLinkNodeRawMeshBackend('mesh', ids.owner, {});
    backend.stateful = stateful;
    backend.setCompletionHandler((terminal, materialize) =>
      completions.complete(terminal, materialize)
    );
    sessionService = backend.createStreamSessionService({});
    sessionService.start();
  }
  const stream = new framework.ZLinkManagedStream(
    { async bindActor() {}, async unbindActor() {}, disconnectPeer() {} },
    ids.session,
    undefined,
    sessionService,
    nativeBinding ? completions : undefined
  );
  if (nativeBinding) {
    sessionService.lookupActor = (_node, actorId) =>
      backend.observeStateful(OperationKind.ActorLookup, {
        id: 100n,
        promise: Promise.resolve({
          terminalResult: RequestResult.Ok,
          failureCode: 0,
          kindData: {
            kind: 'actorLookupCompletion',
            location: { actor: { actorId, generation: 5n, nodeRid: ids.source } }
          }
        })
      });
  }
  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actorId = hooks.actorId ?? 'opaque-relocation-actor';
  await context.actors.bindOrGet({
    actorId,
    objectGeneration: 5n,
    meshName: 'mesh',
    nodeRid: ids.source,
    bindingGeneration: 6n
  });
  const authority = (rid, ownerGeneration, leaseGeneration, nodeGeneration) => ({
    kind: 'snapshot',
    objectGeneration: 5n,
    authorityOwnerGeneration: ownerGeneration,
    ownerLeaseGeneration: leaseGeneration,
    ownerId: 'actor-owner',
    storeVersion: { value: 'v1' },
    allocation: { descriptor: { rid }, descriptorLifecycleGeneration: nodeGeneration }
  });
  let currentAuthority = authority(ids.source, 11n, 13n, 2n);
  const abort = new AbortController();
  const controls = [];
  const tasks = [];
  let sourceRuntime;
  let sessionRuntime;
  const dispatch = (runtime, sourceNodeRid, bytes) =>
    runtime.tryHandleControl(
      'mesh',
      {
        sourceNodeRid,
        parts: [{ data: () => Buffer.from(bytes), close() {} }]
      },
      abort.signal
    );
  const submit = (runtime, sourceNodeRid, bytes) => {
    controls.push(bytes[3]);
    const task = Promise.resolve(dispatch(runtime, sourceNodeRid, bytes));
    tasks.push(task);
    task.catch((error) => {
      if (hooks.onDrop) hooks.onDrop(error);
      else abort.abort(error);
    });
    return SubmitResult.Ok;
  };
  sourceRuntime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({
      status: () => ({ routingId: ids.source, lifecycleGeneration: 2n }),
      sendToNode: (_target, bytes) => submit(sessionRuntime, ids.source, bytes)
    })
  });
  sessionRuntime = new ZLinkHostServiceRelocationRuntime({
    currentOwner: () => ({ ownerId: 'session-owner-id', leaseGeneration: 8n }),
    meshNode: () => ({
      status: () => ({ routingId: ids.owner, lifecycleGeneration: 4n }),
      peers: () => [
        { routingId: ids.source, lifecycleGeneration: 2n },
        { routingId: ids.target, lifecycleGeneration: 3n }
      ],
      sendToNode: (_target, bytes) => submit(sourceRuntime, ids.owner, bytes)
    }),
    boundSessionRelocation: {
      receiveSeal: async (value, signal) => {
        await hooks.beforeSeal?.();
        return host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(
          value,
          signal
        );
      },
      receiveRoute: (value) =>
        host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(value),
      clear: () => host.boundSessionRelay.boundSessions.clearServiceWireSessionRelocations()
    }
  });
  const sent = [];
  const transfer = new ZLinkActorTransferRuntime({
    primaryMeshNode: () => ({ status: () => ({ routingId: ids.source, lifecycleGeneration: 2n }) }),
    authorityStore: () => ({ readAuthority: async () => currentAuthority }),
    liveDescriptors: async () => [
      { rid: ids.owner, lifecycleGeneration: 4n, ownerId: 'session-owner-id', leaseGeneration: 8n }
    ],
    sessionRelocationWire: () => ({
      requestSessionRelocationSeal: (mesh, target, request, signal) => {
        sent.push(request);
        return sourceRuntime.requestSessionRelocationSeal(mesh, target, request, signal);
      },
      sendSessionRelocationRoute: async (_mesh, _target, route) => {
        sent.push(route);
        await dispatch(
          sessionRuntime,
          route.route.action === 'commit' ? ids.target : ids.source,
          wire.encodeSessionRelocationRoute(route)
        );
      }
    })
  });
  const actor = { context: { actorId } };
  const state = {
    nativeActorRef: { actorId, generation: 5n, nodeRid: ids.source },
    locationGeneration: 11n,
    ownerLeaseGeneration: 13n,
    boundSession: {
      routerChannelId: 'mesh',
      targetNodeRid: ids.owner,
      sessionRid: ids.session,
      bindingGeneration: 6n
    }
  };
  if (nativeBinding)
    state.boundSession.sessionRid = sessionService.bindings(ids.session)[0].sessionRid;
  return {
    host,
    stream,
    context,
    abort,
    bindWireSession,
    actor,
    state,
    transfer,
    sent,
    controls,
    seal: () => transfer.sealBoundSessionRoute(actor, state, abort.signal, { high: 7n, low: 9n }),
    receiveSeal: (request) =>
      dispatch(sessionRuntime, ids.source, wire.encodeSessionRelocationSeal(request)),
    commit: async (sealed) => {
      currentAuthority = authority(ids.target, 12n, 14n, 3n);
      await transfer.publishBoundSessionOwnership(
        actorId,
        { actorId, generation: 5n, nodeRid: ids.target },
        12n,
        sealed,
        14n
      );
    },
    close: async () => {
      await sourceRuntime.dispose();
      await sessionRuntime.dispose();
      await Promise.allSettled(tasks);
      sessionService?.close();
      await backend?.close();
    }
  };
}

const binaryIds = {
  source: RoutingId.fromHex('00010203'),
  owner: RoutingId.fromHex('ff000102'),
  session: RoutingId.fromHex('000102030405060708090a0b0c0d0e0f'),
  target: RoutingId.fromHex('fe000103')
};

for (const [name, ids] of Object.entries({
  text: { source: 'source', owner: 'session-owner', session: 'session', target: 'target' },
  binary: binaryIds
})) {
  test(`Session seal ACK and route commit preserve ${name} RID bytes`, async () => {
    const h = await relocationHarness(ids);
    try {
      const sealed = await h.seal();
      const request = h.sent[0];
      assert.equal(request.coordinator.nodeRid, ids.source, 'source RoutingId is preserved');
      assert.equal(request.session.sessionOwnerNodeRid, ids.owner, 'owner RoutingId is preserved');
      assert.equal(request.session.sessionRid, ids.session, 'Session RoutingId is preserved');
      for (const [actual, expected] of [
        [request.actor.actor.nodeRid, ids.source],
        [request.coordinator.nodeRid, ids.source],
        [request.session.sessionOwnerNodeRid, ids.owner],
        [request.session.sessionRid, ids.session]
      ]) {
        assert.equal(encodeRoutingIdStorageHex(actual), encodeRoutingIdStorageHex(expected));
      }
      assert.equal(
        await h.receiveSeal(request),
        true,
        'byte-identical duplicate seal joins its ACK'
      );
      await h.commit(sealed);
      assert.equal(h.sent.at(-1).actor.nodeRid, ids.target, 'commit retains the target RoutingId');
      assert.equal(h.sent.at(-1).route.targetNodeRid, ids.target);
      assert.equal(
        encodeRoutingIdStorageHex(
          (await h.host.streamBindingRuntime.find(h.actor.context.actorId)).ref.nodeRid
        ),
        encodeRoutingIdStorageHex(ids.target)
      );
      assert.equal(
        await h.host.streamBindingRuntime.validateActorRouteSeal(
          h.actor.context.actorId,
          sealed.relocationSealId
        ),
        false
      );
      assert.deepEqual(h.controls, [
        wire.M6bServiceWireCommand.sessionRelocationSeal,
        wire.M6bServiceWireCommand.sessionRelocationSealed,
        wire.M6bServiceWireCommand.sessionRelocationSealed
      ]);
    } finally {
      await h.close();
    }
  });
}

test('binary Session seal abort preserves its identity and releases the source route', async () => {
  const h = await relocationHarness(binaryIds);
  try {
    const sealed = await h.seal();
    await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, sealed);
    assert.equal(
      h.sent.at(-1).actor.nodeRid,
      binaryIds.source,
      'abort retains the source RoutingId'
    );
    assert.equal(
      await h.host.streamBindingRuntime.validateActorRouteSeal(
        h.actor.context.actorId,
        sealed.relocationSealId
      ),
      false
    );
    assert.equal(
      encodeRoutingIdStorageHex(
        (await h.host.streamBindingRuntime.find(h.actor.context.actorId)).ref.nodeRid
      ),
      binaryIds.source.toHex()
    );
  } finally {
    await h.close();
  }
});

for (const mismatch of ['session', 'binding', 'actor']) {
  test(`Session seal rejects a changed ${mismatch} identity after binary wire round-trip`, async () => {
    const h = await relocationHarness(binaryIds);
    try {
      await h.seal();
      const request = h.sent[0];
      const stale = {
        ...request,
        relocation: { high: 8n, low: 9n },
        actor: {
          ...request.actor,
          actor: { ...request.actor.actor, generation: mismatch === 'actor' ? 4n : 5n }
        },
        session: {
          ...request.session,
          sessionRid: mismatch === 'session' ? String(binaryIds.session) : binaryIds.session,
          bindingGeneration: mismatch === 'binding' ? 7n : 6n
        }
      };
      await assert.rejects(h.receiveSeal(stale), framework.ZLinkRemoteBoundSessionFenceError);
    } finally {
      await h.close();
    }
  });
}

test('native Session bind preserves RID 00000001 through command 38 and relocation command 42', async () => {
  const ids = {
    source: 'source',
    owner: 'session-owner',
    target: 'target',
    session: RoutingId.fromHex('00000001')
  };
  const h = await relocationHarness(ids, true);
  try {
    const [outcome] = await Promise.allSettled([h.seal()]);
    const decoded = wire.decodeSessionRelocationSeal(wire.encodeSessionRelocationSeal(h.sent[0]));
    assert.equal(encodeRoutingIdStorageHex(decoded.session.sessionRid), '00000001');
    assert.equal(encodeRoutingIdStorageHex(h.bindWireSession), '00000001');
    assert.equal(outcome.status, 'fulfilled', outcome.reason?.stack);
    await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, outcome.value);
  } finally {
    await h.close();
  }
});

function barrier() {
  let release;
  const joined = new Promise((resolve) => {
    release = resolve;
  });
  return { joined, release };
}

const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const peerOrder of ['none', 'before failure', 'after failure']) {
  test(`aggregate capture restores the Spot and every prepared Session (${peerOrder})`, async () => {
    const entered = barrier();
    const allowSeal = barrier();
    const dropped = barrier();
    const closedTerminal = barrier();
    const allowPeer = barrier();
    const peerPrepared = barrier();
    const closed = await relocationHarness(
      { source: 'source', owner: 'session-owner', session: 'closed-session', target: 'target' },
      false,
      {
        beforeSeal: async () => {
          entered.release();
          await allowSeal.joined;
        },
        onDrop: (error) => dropped.release(error)
      }
    );
    const peer =
      peerOrder === 'none'
        ? undefined
        : await relocationHarness(
            { source: 'source', owner: 'session-owner', session: 'live-session', target: 'target' },
            false,
            {
              actorId: 'live-peer',
              beforeSeal: () => allowPeer.joined
            }
          );
    const events = [];
    let peerSeal;
    const activation = {
      spotId: 'spot',
      serial: new ZLinkSpotSerialTurnExecutor(),
      captureRelocation() {
        events.push('spot:capture');
        return Promise.resolve({ timers: [] });
      },
      abortRelocation() {
        events.push('spot:abort');
      }
    };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      locationStore: () => ({
        readAuthority: async () => ({
          kind: 'snapshot',
          storeVersion: { value: 'v1' },
          payload: Buffer.alloc(0),
          objectGeneration: 5n,
          authorityOwnerGeneration: 11n,
          ownerId: 'owner',
          ownerLeaseGeneration: 13n,
          allocation: { state: 'active', objectKind: 'user_spot', stableType: 'Spot' },
          storeNow: new Date()
        })
      }),
      spotManager: () => ({
        pendingSpotCloseDecision: () => undefined,
        isSpotClosing: () => false
      }),
      meshNode: () => ({
        status: () => ({ routingId: 'source', lifecycleGeneration: 2n }),
        sealSpotMessageFollowIngress() {
          events.push('spot:follow-seal');
          return { spotId: 'spot' };
        },
        abortSpotMessageFollowIngress() {
          events.push('spot:follow-abort');
          return true;
        },
        commitSpotMessageFollowIngress() {}
      }),
      actorTransfer: {
        async prepareMaintenanceSession(actor) {
          if (actor === closed.actor) {
            try {
              await closed.seal();
            } finally {
              closedTerminal.release();
            }
            throw new Error('the closed Session seal unexpectedly succeeded');
          }
          peerSeal = await peer.seal();
          events.push('peer:prepared');
          peerPrepared.release();
          return {
            async rollback() {
              events.push('peer:rollback');
              await peer.transfer.abortBoundSessionRouteSeal(peer.actor, peer.state, peerSeal);
            }
          };
        }
      }
    });
    runtime.spotRegistration = () => ({ relocation: { kind: 'allowed' } });
    runtime.actorRegistration = () => ({ relocation: { kind: 'allowed' } });
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.runCoordinator = async () => {
      events.push('target:prepare');
    };
    const states = [closed, ...(peer ? [peer] : [])].map((h) => ({
      actorId: h.actor.context.actorId,
      actor: h.actor,
      actorType: 'Actor',
      spotMembershipEpoch: 1n
    }));
    const failure = new Error('release the failed seal at its terminal boundary');
    const operation = runtime.relocateSpotAggregate(
      'mesh',
      activation,
      'user_spot',
      states,
      { rid: 'target' },
      undefined,
      closed.abort.signal
    );
    const outcome = operation.then(
      () => null,
      (error) => error
    );
    try {
      await Promise.race([
        entered.joined,
        outcome.then((error) => {
          throw error ?? new Error('capture ended before seal');
        })
      ]);
      await closed.stream.close();
      await closed.host.streamBindingRuntime.cleanup(closed.context);
      assert.equal(
        await closed.host.streamBindingRuntime.sessionRouteFence(closed.actor.context.actorId),
        undefined
      );
      if (peerOrder === 'before failure') {
        allowPeer.release();
        await peerPrepared.joined;
      }
      allowSeal.release();
      assert.ok((await dropped.joined) instanceof framework.ZLinkRemoteBoundSessionFenceError);
      closed.abort.abort(failure);
      await closedTerminal.joined;
      if (peerOrder === 'after failure') {
        allowPeer.release();
        await peerPrepared.joined;
      }
      assert.equal(await outcome, failure);
      assert.equal(
        events.filter((event) => event === 'spot:abort').length,
        1,
        'partially captured Spot must be restored'
      );
      assert.equal(
        events.filter((event) => event === 'spot:follow-abort').length,
        1,
        'Message Follow ingress must be restored'
      );
      assert.equal(events.includes('target:prepare'), false);
      if (peer) {
        assert.equal(
          events.filter((event) => event === 'peer:rollback').length,
          1,
          'every successful participant must be restored, including a late completion'
        );
        assert.equal(
          await peer.host.streamBindingRuntime.validateActorRouteSeal(
            peer.actor.context.actorId,
            peerSeal.relocationSealId
          ),
          false
        );
        assert.equal(peer.sent.at(-1).route.action, 'abort');
      }
    } finally {
      allowSeal.release();
      allowPeer.release();
      closed.abort.abort(failure);
      await outcome;
      await closed.close();
      await peer?.close();
      await runtime.dispose();
    }
  });
}
