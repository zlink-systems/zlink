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
  ServiceWireProtocolError
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
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
  if (hooks.notifyDisconnected)
    host.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({
      notifyDisconnected: hooks.notifyDisconnected
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
    bindingGeneration: 6n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 2n
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
  const responses = [];
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
    if (bytes[3] === wire.M6bServiceWireCommand.sessionRelocationSealed)
      responses.push(wire.decodeSessionRelocationSealed(bytes));
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
    responses,
    sourceRuntime,
    sessionRuntime,
    dispatch,
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

async function rebind(h, bindingGeneration = 7n) {
  await h.stream.close();
  await h.host.streamBindingRuntime.cleanup(h.context);
  const context = h.host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(
      { async bindActor() {}, async unbindActor() {}, disconnectPeer() {} },
      'successor-session'
    )
  );
  await context.actors.bindOrGet({
    actorId: h.actor.context.actorId,
    objectGeneration: 5n,
    meshName: 'mesh',
    nodeRid: 'source',
    bindingGeneration,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 2n
  });
  return context;
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

// #1883: port of closed-session-seal-repro.cjs, with owner ingress held at
// a deterministic barrier and cooperative release/join on every exit.
test('closed Session seal returns bindingAbsent and relocation reaches target preparation', async () => {
  const entered = barrier();
  const allowSeal = barrier();
  const dropped = barrier();
  const h = await relocationHarness(
    { source: 'source', owner: 'session-owner', session: 'session', target: 'target' },
    false,
    {
      beforeSeal: async () => {
        entered.release();
        await allowSeal.joined;
      },
      onDrop: (error) => dropped.release(error)
    }
  );
  const source = h.seal();
  const joined = source.then(
    (value) => ({ value }),
    (error) => ({ error })
  );
  try {
    await Promise.race([
      entered.joined,
      joined.then((result) => {
        throw result.error ?? new Error('seal completed before the owner barrier');
      })
    ]);
    await h.stream.close();
    await h.host.streamBindingRuntime.cleanup(h.context);
    assert.equal(
      await h.host.streamBindingRuntime.sessionRouteFence(h.actor.context.actorId),
      undefined
    );
    allowSeal.release();
    const result = await Promise.race([joined, dropped.joined.then((error) => ({ error }))]);
    assert.equal(result.error, undefined, result.error?.stack);
    assert.equal(
      result.value,
      undefined,
      'absent participant has no sessionBindings entry or command 44 target'
    );
    assert.deepEqual(h.controls, [
      wire.M6bServiceWireCommand.sessionRelocationSeal,
      wire.M6bServiceWireCommand.sessionRelocationSealed
    ]);
    await h.receiveSeal(h.sent[0]);
    assert.equal(
      h.controls.filter((command) => command === wire.M6bServiceWireCommand.sessionRelocationSealed)
        .length,
      2
    );
  } finally {
    allowSeal.release();
    h.abort.abort(new Error('cooperative test release'));
    await joined;
    await h.close();
  }
});

const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const peerOrder of ['none', 'before failure', 'after failure']) {
  test(`binding mismatch keeps the Spot dispatching and restores every prepared Session (${peerOrder})`, async () => {
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
            throw new Error('the mismatched Session seal unexpectedly succeeded');
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
      await rebind(closed);
      assert.equal(
        (await closed.host.streamBindingRuntime.sessionRouteFence(closed.actor.context.actorId))
          .bindingGeneration,
        7n
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
      // Relocation 04 §4.1: member Sessions are sealed before the Spot stops
      // dispatch, so a failed member seal leaves the Spot dispatching.
      assert.deepEqual(
        events.filter((event) => event.startsWith('spot:')),
        [],
        'a failed member seal must not stop Spot dispatch or Message Follow ingress'
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

test('Session owner records each participant and rejects any changed command 42 field', async () => {
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  const relay = h.host.boundSessionRelay.boundSessions;
  try {
    await h.seal();
    const request = h.sent[0];
    const participant = {
      ...request,
      actor: { ...request.actor, actor: { ...request.actor.actor, actorId: 'other-participant' } }
    };
    const absent = await relay.receiveServiceWireSessionRelocationSeal(participant);
    assert.equal(absent.sealResult, 'bindingAbsent');
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(request)).sealResult,
      'sealed'
    );
    assert.deepEqual(await relay.receiveServiceWireSessionRelocationSeal(participant), absent);
    const mutations = [
      { ...request, coordinator: { ...request.coordinator, ownerId: 'other-owner' } },
      { ...request, session: { ...request.session, sessionRid: 'other-session' } },
      { ...request, session: { ...request.session, bindingGeneration: 7n } },
      {
        ...request,
        actor: { ...request.actor, actor: { ...request.actor.actor, generation: 6n } }
      },
      { ...participant, actor: { ...participant.actor, ownerLeaseGeneration: 14n } }
    ];
    for (const mutation of mutations) {
      await assert.rejects(
        relay.receiveServiceWireSessionRelocationSeal(mutation),
        (error) =>
          error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
      );
    }
  } finally {
    await h.close();
  }
});

test('closed seal ignores duplicate 42 without a reply and disappears with its binding', async () => {
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  const warnings = [];
  const originalWarn = console.warn;
  console.warn = (value) => warnings.push(value);
  try {
    const sealed = await h.seal();
    await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, sealed);
    const count = h.responses.length;
    await h.receiveSeal(h.sent[0]);
    assert.equal(h.responses.length, count, 'closed seal sends no second command 43');
    assert.ok(warnings.some((value) => value.startsWith('late_session_relocation_seal ')));
    await assert.rejects(
      h.receiveSeal({ ...h.sent[0], session: { ...h.sent[0].session, bindingGeneration: 7n } }),
      (error) => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
    );
    const successor = await rebind(h);
    await assert.rejects(h.receiveSeal(h.sent[0]), framework.ZLinkRemoteBoundSessionFenceError);
    await successor.stream.close();
    await h.host.streamBindingRuntime.cleanup(successor);
    await h.receiveSeal(h.sent[0]);
    assert.equal(h.responses.at(-1).sealResult, 'bindingAbsent');
  } finally {
    console.warn = originalWarn;
    await h.close();
  }
});

test('bindingAbsent expires at the configured boundary and late 44 cannot touch a rebind', async (t) => {
  const {
    zlinkDefaultLocationOptions
  } = require('../../packages/framework/dist/contracts/Locations/Options');
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  const relay = h.host.boundSessionRelay.boundSessions;
  try {
    const sealed = await h.seal();
    const request = h.sent[0];
    await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, sealed);
    await h.stream.close();
    await h.host.streamBindingRuntime.cleanup(h.context);
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(request)).sealResult,
      'bindingAbsent'
    );
    await rebind(h);
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(request)).sealResult,
      'bindingAbsent',
      'record is used before inspecting the successor'
    );
    const successor = {
      ...request,
      relocation: { high: 7n, low: 10n },
      session: { ...request.session, sessionRid: 'successor-session', bindingGeneration: 7n }
    };
    await relay.receiveServiceWireSessionRelocationSeal(successor);
    const late = {
      relocation: request.relocation,
      coordinator: request.coordinator,
      senderRole: 'source',
      actor: request.actor.actor,
      session: request.session,
      route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
    };
    await relay.receiveServiceWireSessionRelocationRoute(late);
    assert.equal(
      await h.host.streamBindingRuntime.validateActorRouteSeal(
        h.actor.context.actorId,
        wire.serviceSessionRelocationIdentityKey(successor)
      ),
      true
    );
    await relay.receiveServiceWireSessionRelocationRoute({
      ...late,
      relocation: successor.relocation,
      session: successor.session
    });
    t.mock.timers.tick(zlinkDefaultLocationOptions.sessionRelocationSealTimeoutMs);
    await assert.rejects(
      relay.receiveServiceWireSessionRelocationSeal(request),
      framework.ZLinkRemoteBoundSessionFenceError
    );
    assert.equal(
      (await h.host.streamBindingRuntime.sessionRouteFence(h.actor.context.actorId))
        .bindingGeneration,
      7n
    );
  } finally {
    await h.close();
    t.mock.timers.reset();
  }
});

test('command 42 with a stale owner lease is rejected after the previous closed record is replaced', async () => {
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  try {
    const sealed = await h.seal();
    await h.commit(sealed);
    const stale = { ...h.sent[0], relocation: { high: 8n, low: 9n } };
    await assert.rejects(
      h.host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(stale),
      framework.ZLinkRemoteBoundSessionFenceError
    );
    const current = {
      ...stale,
      actor: {
        ...stale.actor,
        actor: { ...stale.actor.actor, nodeRid: 'target' },
        targetNodeGeneration: 3n,
        authorityOwnerGeneration: 12n,
        ownerLeaseGeneration: 14n
      }
    };
    assert.equal(
      (
        await h.host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(
          current
        )
      ).sealResult,
      'sealed'
    );
  } finally {
    await h.close();
  }
});

test('coordinator uses the first 43 per participant and never resends a terminal 42', async () => {
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  try {
    await h.seal();
    const first = h.responses[0];
    await h.dispatch(
      h.sourceRuntime,
      'session-owner',
      wire.encodeSessionRelocationSealed({ ...first, sealResult: 'bindingAbsent' })
    );
    const count = h.controls.length;
    assert.deepEqual(
      await h.sourceRuntime.requestSessionRelocationSeal('mesh', 'session-owner', h.sent[0]),
      first
    );
    assert.equal(h.controls.length, count);
    const peer = {
      ...h.sent[0],
      actor: { ...h.sent[0].actor, actor: { ...h.sent[0].actor.actor, actorId: 'absent-peer' } }
    };
    const absent = await h.sourceRuntime.requestSessionRelocationSeal(
      'mesh',
      'session-owner',
      peer
    );
    assert.equal(absent.sealResult, 'bindingAbsent');
    await h.dispatch(
      h.sourceRuntime,
      'session-owner',
      wire.encodeSessionRelocationSealed({ ...absent, sealResult: 'sealed' })
    );
    assert.deepEqual(
      await h.sourceRuntime.requestSessionRelocationSeal('mesh', 'session-owner', peer),
      absent
    );
  } finally {
    await h.close();
  }
});

for (const binding of ['absent', 'same', 'successor']) {
  test(`in-flight disconnect runs once with ${binding} Actor binding`, async () => {
    const {
      ZLinkActorPacketRelay
    } = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
    const packets = require('../../packages/framework/dist/runtime/actors/actor-packet-relay-wire');
    const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
    const {
      encodeSessionDisconnected
    } = require('../../packages/framework/dist/runtime/actors/session-disconnected-wire');
    const {
      DefaultZLinkActorContext
    } = require('../../packages/framework/dist/runtime/actors/actor-context');
    const {
      ZLinkSpotActorPacketDispatch
    } = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-dispatch');
    const entered = barrier();
    const allowNotification = barrier();
    let currentBinding = {
      sessionNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      bindingGeneration: 6n
    };
    let callbacks = 0;
    let submissions = 0;
    const actorRelay = new ZLinkActorPacketRelay({
      meshRouters: {},
      streamBindingRuntime: () => ({}),
      actorManager: () => ({
        getState: () => ({ boundSession: currentBinding, spotId: 'spot', actor: {} })
      }),
      spotManager: () => ({
        async dispatchRoutedActorPacket(origin, _spotId, actorId, parts) {
          const state = new framework.ZLinkActorRuntimeState(actorId);
          if (currentBinding !== undefined)
            state.installBoundSessionBinding({
              routerChannelId: 'route',
              targetNodeRid: 'session-owner',
              spotId: 'entry',
              sessionRid: 'session',
              sessionOwnerId: 'session-owner-id',
              sessionOwnerLeaseGeneration: 8n,
              ...currentBinding
            });
          return new ZLinkSpotActorPacketDispatch({
            spot: {},
            spotId: () => 'spot',
            registry: {},
            resolveActor: () => ({ context: new DefaultZLinkActorContext(state) }),
            onDisconnectActor: async () => {
              callbacks++;
            }
          }).dispatch(origin, actorId, parts);
        }
      })
    });
    const h = await relocationHarness(
      { source: 'source', owner: 'session-owner', session: 'session', target: 'target' },
      false,
      {
        async notifyDisconnected(actor) {
          submissions++;
          const payload = packets.encodeRemoteActorPacketRelayPayload({
            actorId: actor.actorId,
            bindingActorRef: actor.ref,
            header: protocol.encodeStreamHeader({
              kind: protocol.ZLinkStreamMessageKind.Send,
              codec: protocol.ZLinkStreamCodec.Raw,
              flags: protocol.ZLinkStreamHeaderFlags.None,
              name: packets.ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
              metadata: new Map()
            }),
            payload: encodeSessionDisconnected('session-owner', 4n, 6n)
          });
          entered.release();
          await allowNotification.joined;
          const result = await actorRelay.receiveRemoteActorPacketRelay(payload, {
            sourceNodeRid: 'session-owner'
          });
          assert.equal(result.ok, true);
        }
      }
    );
    let cleanup;
    try {
      // Capture the original request while the Session still exists, then
      // abort that seal so physical cleanup can race the next relocation.
      const sealed = await h.seal();
      await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, sealed);
      await h.stream.close();
      cleanup = h.host.streamBindingRuntime.cleanup(h.context);
      await Promise.race([
        entered.joined,
        cleanup.then(() => {
          throw new Error('cleanup completed before notification admission');
        })
      ]);
      currentBinding =
        binding === 'absent'
          ? undefined
          : binding === 'same'
            ? currentBinding
            : {
                sessionNodeRid: 'session-owner',
                sessionOwnerNodeGeneration: 4n,
                bindingGeneration: 7n
              };
      const absent =
        await h.host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(
          h.sent[0]
        );
      assert.equal(absent.sealResult, 'bindingAbsent');
      await h.host.streamBindingRuntime.cleanup(h.context);
      assert.equal(callbacks, 0, 'notification is still held after absence was acknowledged');
      allowNotification.release();
      await cleanup;
      await h.host.streamBindingRuntime.cleanup(h.context);
      assert.equal(submissions, 1, 'Session owner submits at most once per retired binding');
      assert.equal(callbacks, binding === 'successor' ? 0 : 1);
    } finally {
      allowNotification.release();
      await cleanup;
      await h.close();
    }
  });
}

test('aggregate reaches coordinator with both Actors and membership but only the live Session binding', async () => {
  const entered = barrier();
  const allowSeal = barrier();
  const dropped = barrier();
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
  const live = await relocationHarness(
    { source: 'source', owner: 'session-owner', session: 'live-session', target: 'target' },
    false,
    { actorId: 'live-participant' }
  );
  let envelope;
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
        allocation: { state: 'active', objectKind: 'user_spot', stableType: 'Unit' },
        storeNow: new Date()
      })
    }),
    spotManager: () => ({ pendingSpotCloseDecision: () => undefined, isSpotClosing: () => false }),
    meshNode: () => ({
      status: () => ({ routingId: 'source', lifecycleGeneration: 2n }),
      sealSpotMessageFollowIngress: () => ({ spotId: 'spot' }),
      abortSpotMessageFollowIngress: () => true,
      commitSpotMessageFollowIngress() {}
    }),
    actorTransfer: {
      async prepareMaintenanceSession(actor) {
        const h = actor === closed.actor ? closed : live;
        const target = await h.seal();
        return {
          target: target && { ...target, sessionNodeRid: 'session-owner' },
          handoffBacklog: [],
          async rollback() {
            if (target) await h.transfer.abortBoundSessionRouteSeal(h.actor, h.state, target);
          }
        };
      }
    }
  });
  runtime.spotRegistration = runtime.actorRegistration = () => ({
    relocation: { kind: 'allowed' }
  });
  runtime.waitForRelocationBudgetHeadroom = async () => {};
  runtime.captureApplication = async () => Buffer.alloc(0);
  runtime.runCoordinator = async (_mesh, _target, _authority, captured) => {
    envelope = captured.envelope;
    // The target-preparation boundary has been reached. Release captures
    // cooperatively rather than creating a fake committed authority.
    await captured.abortSource();
  };
  const activation = {
    spotId: 'spot',
    serial: new ZLinkSpotSerialTurnExecutor(),
    captureRelocation: async () => ({ timers: [] }),
    abortRelocation() {}
  };
  const states = [closed, live].map((h) => ({
    actorId: h.actor.context.actorId,
    actor: h.actor,
    actorType: 'Actor',
    spotMembershipEpoch: 1n
  }));
  const operation = runtime.relocateSpotAggregate(
    'mesh',
    activation,
    'user_spot',
    states,
    { rid: 'target' },
    undefined,
    closed.abort.signal
  );
  const joined = operation.then(
    () => ({ ok: true }),
    (error) => ({ error })
  );
  try {
    await Promise.race([
      entered.joined,
      joined.then((result) => {
        throw result.error ?? new Error('aggregate ended before seal');
      })
    ]);
    await closed.stream.close();
    await closed.host.streamBindingRuntime.cleanup(closed.context);
    allowSeal.release();
    const outcome = await Promise.race([joined, dropped.joined.then((error) => ({ error }))]);
    assert.equal(outcome.error, undefined, outcome.error?.stack);
    assert.equal(outcome.ok, true);
    assert.equal(envelope.participants.filter((p) => p.objectKind === 'actor').length, 2);
    assert.equal(envelope.memberships.length, 2);
    const bindings = envelope.participants.filter((p) => p.sessionBinding !== undefined);
    assert.equal(bindings.length, 1);
    assert.equal(Buffer.from(bindings[0].sessionBinding.sessionRid).toString(), 'live-session');
    assert.equal(closed.responses[0].sealResult, 'bindingAbsent');
    assert.equal(closed.sent.length, 1, 'absent participant never receives command 44');
    assert.equal(live.sent.at(-1).route.action, 'abort', 'only confirmed seal is restored');
  } finally {
    allowSeal.release();
    closed.abort.abort(new Error('cooperative aggregate release'));
    await joined;
    await runtime.dispose();
    await closed.close();
    await live.close();
  }
});

test('retired active seal late 44 and expiry preserve a successor relocation', async (t) => {
  const {
    zlinkDefaultLocationOptions
  } = require('../../packages/framework/dist/contracts/Locations/Options');
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const h = await relocationHarness({
    source: 'source',
    owner: 'session-owner',
    session: 'session',
    target: 'target'
  });
  const relay = h.host.boundSessionRelay.boundSessions;
  let successor;
  try {
    await h.seal();
    const request = h.sent[0];
    await h.stream.close();
    await h.host.streamBindingRuntime.cleanup(h.context);
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(request)).sealResult,
      'bindingAbsent'
    );
    await rebind(h);
    t.mock.timers.tick(1);
    successor = {
      ...request,
      relocation: { high: 8n, low: 9n },
      session: { ...request.session, sessionRid: 'successor-session', bindingGeneration: 7n }
    };
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(successor)).sealResult,
      'sealed'
    );
    await relay.receiveServiceWireSessionRelocationRoute({
      relocation: request.relocation,
      coordinator: request.coordinator,
      senderRole: 'source',
      actor: request.actor.actor,
      session: request.session,
      route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
    });
    const successorKey = wire.serviceSessionRelocationIdentityKey(successor);
    assert.equal(
      await h.host.streamBindingRuntime.validateActorRouteSeal(
        h.actor.context.actorId,
        successorKey
      ),
      true
    );
    t.mock.timers.tick(zlinkDefaultLocationOptions.sessionRelocationSealTimeoutMs - 1);
    // Join owner work from the retired timer, without advancing the new seal's deadline.
    assert.equal(
      (await h.host.streamBindingRuntime.sessionRouteFence(h.actor.context.actorId))
        .bindingGeneration,
      7n
    );
    assert.equal(
      await h.host.streamBindingRuntime.validateActorRouteSeal(
        h.actor.context.actorId,
        successorKey
      ),
      true
    );
    assert.equal(
      (await relay.receiveServiceWireSessionRelocationSeal(successor)).sealResult,
      'sealed'
    );
  } finally {
    if (successor)
      await relay.receiveServiceWireSessionRelocationRoute({
        relocation: successor.relocation,
        coordinator: successor.coordinator,
        senderRole: 'source',
        actor: successor.actor.actor,
        session: successor.session,
        route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
      });
    await h.close();
    t.mock.timers.reset();
  }
});
