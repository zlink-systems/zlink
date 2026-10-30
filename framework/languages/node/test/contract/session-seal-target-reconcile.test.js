const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { Message } = require('@zlink-systems/zlink');
const { createRelayHost } = require('./fixtures/relay-host');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

async function fixture({ ready = false, target = 'target', absent = false, records = [] } = {}) {
  const registration = framework.createFrameworkRegistration();
  const { host, relay } = createRelayHost({ registration });
  host.routeTransport.requestRawToSpot = async () => [
    Message.from(JSON.stringify({ ok: true, response: { acknowledged: true } }))
  ];
  const context = host.streamBindingRuntime.createSessionContext({
    sessionId: 'session',
    routingId: 'session',
    write: () => true,
    writeRaw: () => true,
    async close() {}
  });
  if (!absent)
    await context.actors.bind({
      nodeRid: 'source',
      actorId: 'actor',
      generation: 5n,
      ownershipGeneration: 11n,
      ownerLeaseGeneration: 13n,
      bindingGeneration: 6n
    });
  const requests = [];
  let requestSeen;
  const requested = new Promise((resolve) => {
    requestSeen = resolve;
  });
  const responses = [];
  const node = {
    status: () => ({ routingId: 'session-owner', lifecycleGeneration: 4n }),
    sendInfrastructureControl(_target, bytes) {
      responses.push(wire.decodeSessionRelocationSealed(bytes));
      return SubmitResult.Ok;
    },
    peers: () => [{ routingId: 'source', lifecycleGeneration: 2n }],
    isPeerRouteReady(rid, generation) {
      assert.equal(String(rid), target);
      assert.equal(generation, 9n);
      return ready;
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration,
    messageFlow: () => ({ begin: () => ({ trace: (event) => records.push(event) }) }),
    boundSessionRelocation: {
      receiveSeal: (...args) =>
        relay.boundSessions.receiveServiceWireSessionRelocationSeal(...args),
      receiveRoute: (value) => relay.boundSessions.receiveServiceWireSessionRelocationRoute(value),
      clear: () => relay.boundSessions.clearServiceWireSessionRelocations()
    },
    meshNode: () => node,
    currentOwner: () => ({ ownerId: 'session-owner-id', leaseGeneration: 8n }),
    requestPeerReconcile(meshName, peerRid) {
      requests.push([meshName, peerRid]);
      requestSeen();
      // An unfinished reconcile must not hold the seal result or command 43.
      return new Promise(() => {});
    }
  });
  const seal = {
    relocation: { high: 1n, low: 2n },
    coordinator: {
      ownerId: 'source-owner',
      leaseGeneration: 13n,
      nodeRid: 'source',
      nodeGeneration: 2n,
      expectedAuthorityStoreVersion: 'v1'
    },
    senderRole: 'source',
    actor: {
      actor: { nodeRid: 'source', actorId: 'actor', generation: 5n },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n
    },
    session: {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      sessionOwnerId: 'session-owner-id',
      sessionOwnerLeaseGeneration: 8n,
      sessionRid: 'session',
      bindingGeneration: 6n
    },
    targetNodeRid: target,
    targetNodeGeneration: 9n
  };
  return {
    host,
    runtime,
    requests,
    requested,
    seal,
    receive: async (value) => {
      const handled = await runtime.tryHandleControl('mesh', {
        sourceNodeRid: 'source',
        parts: [Message.from(wire.encodeSessionRelocationSeal(value ?? seal))]
      });
      assert.equal(handled, true);
      return responses.at(-1);
    },
    close: () => runtime.dispose()
  };
}

test('command 42 immediately reconciles its relocation target after seal installation without awaiting reconcile', async () => {
  const f = await fixture();
  try {
    assert.equal((await f.receive()).sealResult, 'sealed');
    assert.deepEqual(f.requests, [['mesh', 'target']]);
    await f.receive();
    assert.equal(f.requests.length, 1, 'an identical command 42 reuses its installed seal');
  } finally {
    await f.close();
  }
});

for (const options of [{ ready: true }, { target: 'session-owner' }, { absent: true }]) {
  test(`command 42 needs no reconcile for ${JSON.stringify(options)}`, async () => {
    const f = await fixture(options);
    try {
      assert.equal((await f.receive()).sealResult, options.absent ? 'bindingAbsent' : 'sealed');
      assert.deepEqual(f.requests, []);
    } finally {
      await f.close();
    }
  });
}

test('reconcile starts while accepted Session frames still hold command 43', async () => {
  const f = await fixture();
  const registry = f.host.streamBindingRuntime.routes;
  const route = await registry.route('actor');
  const frame = await registry.beginAcceptedFrameWhenReady('actor', route.bindingToken);
  let finished = false;
  const operation = f.receive().then((value) => {
    finished = true;
    return value;
  });
  try {
    await f.requested;
    assert.equal(finished, false, 'command 43 still waits for the existing active frame');
    assert.deepEqual(f.requests, [['mesh', 'target']]);
    await frame.complete();
    assert.equal((await operation).sealResult, 'sealed');
  } finally {
    await frame.complete();
    await f.close();
  }
});

test('a fenced command 42 does not request target reconciliation', async () => {
  const f = await fixture();
  try {
    await assert.rejects(
      f.receive({ ...f.seal, session: { ...f.seal.session, sessionRid: 'wrong-session' } })
    );
    assert.deepEqual(f.requests, []);
  } finally {
    await f.close();
  }
});

test('malformed command 42 records an invalid-frame dispatch error', async () => {
  const records = [];
  const f = await fixture({ records });
  const malformed = Buffer.from([0x5a, 0x4d, 0x01, 42, 0x00]);
  const record = {
    parts: [{ data: () => malformed }],
    sourceNodeRid: 'source'
  };
  try {
    await assert.rejects(f.runtime.tryHandleControl('mesh', record));
    assert.equal(records.length, 1);
    assert.equal(records[0].outcome, 'error');
    assert.equal(records[0].surface, 'streamSession');
    assert.equal(records[0].messageKind, 'control');
    assert.equal(records[0].errorReason, 'invalid_frame');
    assert.equal(records[0].errorAction, 'drop');
  } finally {
    await f.close();
  }
});
