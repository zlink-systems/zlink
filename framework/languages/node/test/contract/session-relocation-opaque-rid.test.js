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

async function relocationHarness(ids) {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const stream = new framework.ZLinkManagedStream(
    { async bindActor() {}, async unbindActor() {} },
    ids.session
  );
  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actorId = 'opaque-relocation-actor';
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
    task.catch((error) => abort.abort(error));
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
      receiveSeal: (value, signal) =>
        host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(value, signal),
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
  return {
    host,
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
