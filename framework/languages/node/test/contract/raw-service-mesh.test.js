const assert = require('node:assert/strict');
const test = require('node:test');
const net = require('node:net');
const { once } = require('node:events');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { wrapSocket } = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { encodeMultipartApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { OperationKind } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

function sessionFixture(disconnectFailure, nativeSocket, routingId = zlink.RoutingId.from(1)) {
  const actor = { nodeRid: 'actor-node', actorId: 'actor-a', generation: 7n };
  const nextActor = { ...actor, actorId: 'actor-b' };
  const fence = { actor, targetNodeGeneration: 3n, authorityOwnerGeneration: 9n, ownerLeaseGeneration: 10n };
  let ingress;
  const stateful = new ServiceStatefulRuntime({
    setServiceIngress(handler) { ingress = handler; },
    async requestService(_target, parts) {
      const record = wire.decodeStatefulHeader(parts[0]);
      assert.equal(record.kind, 'boundSessionBind');
      const binding = record.binding.state === 'active';
      return [wire.encodeStatefulReply(record.correlation, zlink.RequestResult.Ok, 0,
        binding ? { kind: 'streamBind', bindingGeneration: 11n, authorityOwnerGeneration: 9n } : undefined)];
    }
  }, 'session-node', 3n);
  stateful.rememberActorRoute(fence);
  stateful.rememberActorRoute({ ...fence, actor: nextActor });
  const backend = new ZLinkNodeRawMeshBackend('play', 'session-node', {});
  backend.stateful = stateful;
  const completions = new ZLinkMeshCompletionTable();
  backend.readyHandler = () => queueMicrotask(() => {
    let completion;
    while ((completion = backend.takeCompletion()) !== undefined) {
      completions.complete({
        operationId: completion.operationId, operationKind: completion.operationKind,
        terminalResult: completion.result.terminalResult, failureErrno: completion.result.failureCode,
        kindData: completion.result.kindData ?? null, parts: []
      });
    }
  });
  let dropped = false;
  let disconnectCalls = 0;
  const delivered = [];
  const native = {
    close() {},
    disconnectRid(rid) {
      disconnectCalls++;
      if (nativeSocket !== undefined) return nativeSocket.disconnectRid(rid);
      if (disconnectFailure !== undefined) throw disconnectFailure;
    },
    send(rid) {
      if (nativeSocket !== undefined) return nativeSocket.send(rid);
      const parts = [];
      const operation = {
        message(part) { parts.push(Buffer.from(part)); return operation; },
        async submit() {
          if (dropped && String(rid) === String(routingId)) {
            throw new zlink.SubmitError(zlink.SubmitResult.NotFound, 2);
          }
          delivered.push(...parts);
        }
      };
      return operation;
    }
  };
  const service = backend.createStreamSessionService(native);
  service.start();
  service.lookupActor = (_node, actorId) => backend.observeStateful(OperationKind.ActorLookup, {
    id: 100n,
    promise: Promise.resolve({ terminalResult: zlink.RequestResult.Ok, failureCode: 0,
      kindData: { kind: 'actorLookupCompletion', location: { actor: actorId === actor.actorId ? actor : nextActor } } })
  });
  let closed = 0;
  let removed = 0;
  let resolveRemoved;
  const removal = new Promise(resolve => { resolveRemoved = resolve; });
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime();
  const session = new framework.ZLinkStreamSessionRuntime({
    socket: wrapSocket(native), nativeSessionService: service, meshCompletions: completions,
    bindingRuntime,
    sessionFactory(context) {
      return { context, async onDisconnected() { closed++; } };
    }
  }, routingId, () => { removed++; resolveRemoved(); });
  return {
    session, service, bindingRuntime, delivered, removal, nextActor,
    get counts() { return { closed, removed, disconnectCalls }; },
    drop() { dropped = true; },
    async bind() {
      await session.context.actors.bind({ ...actor, objectGeneration: actor.generation, meshName: 'play' });
    },
    async bindNext() {
      const stream = new framework.ZLinkManagedStream(
        wrapSocket(native), zlink.RoutingId.from(2), undefined, service, completions
      );
      await stream.bindActor({ ...nextActor, objectGeneration: nextActor.generation, meshName: 'play' }, 1000);
    },
    deliver(targetActor = actor) {
      return ingress({
        command: wire.M6bServiceWireCommand.boundSessionSend, flags: 0,
        sourceRoutingId: actor.nodeRid, sourceNodeGeneration: fence.targetNodeGeneration,
        parts: [wire.encodeBoundSessionSendHeader({ ...fence, actor: targetActor }, 11n),
          encodeMultipartApplicationPayload([Buffer.from('notice')], 'notice', 'application/octet-stream')]
      });
    },
    async dispose() {
      await session.dispose();
      service.close(); stateful.close(); completions.dispose();
    }
  };
}

for (const absent of [false, true]) {
  test(`raw session delivery close completes through the session owner (RID absent=${absent})`, async () => {
    const fixture = sessionFixture(absent ? new zlink.ConfigError(zlink.ConfigResult.NotFound, 2) : undefined);
    try {
      await fixture.bind();
      await fixture.bindNext();
      assert.equal(await fixture.deliver(), 'application');
      fixture.drop();
      assert.equal(await fixture.deliver(), 'application', 'remote command 36 must not throw out of ingress');
      assert.equal(await fixture.deliver(fixture.nextActor), 'application', 'the same ingress must process the next bound session');
      assert.equal(fixture.delivered.length, 2);
      // The Core monitor already owns the disconnect observation, including
      // when its RID disappeared before the final command 36 was delivered.
      fixture.session.enqueueDisconnected();
      fixture.session.enqueueDisconnected();
      await fixture.removal;
      assert.equal(await fixture.bindingRuntime.find('actor-a'), undefined);
      assert.deepEqual(fixture.session.context.actors.bound, []);
      assert.deepEqual(fixture.counts, { closed: 1, removed: 1, disconnectCalls: 1 });
    } finally {
      await fixture.dispose();
    }
  });
}

test('STREAM explicit close treats an absent RID as completed and cleans the session once', async () => {
  const fixture = sessionFixture(new zlink.ConfigError(zlink.ConfigResult.NotFound, 2));
  try {
    await fixture.bind();
    await fixture.session.context.close();
    fixture.session.enqueueDisconnected();
    await fixture.removal;
    assert.equal(await fixture.bindingRuntime.find('actor-a'), undefined);
    assert.deepEqual(fixture.session.context.actors.bound, []);
    assert.deepEqual(fixture.counts, { closed: 1, removed: 1, disconnectCalls: 1 });
  } finally { await fixture.dispose(); }
});

for (const error of [
  new zlink.ConfigError(zlink.ConfigResult.Busy, 16),
  new zlink.ConfigError(zlink.ConfigResult.InternalError, 2),
  Object.assign(new Error('not a binding ConfigError'), { result: 706, code: 706, nativeErrno: 2 })
]) {
  test(`STREAM close preserves other disconnect failures: ${error.name}/${error.result}`, async () => {
    const fixture = sessionFixture(error);
    try {
      await fixture.bind();
      fixture.drop();
      await assert.rejects(fixture.deliver(), failure => failure === error);
      await assert.rejects(fixture.session.context.close(), failure => failure === error);
    } finally { await fixture.dispose(); }
  });
}


test('raw bound delivery survives a RID already removed by installed Core', { timeout: 5000 }, async () => {
  const context = zlink.createContext();
  const socket = zlink.createStreamSocket(context);
  socket.options.recvMode = zlink.StreamRecvMode.Packet;
  socket.options.linger = 0;
  const packet = new zlink.StreamPacket();
  let client;
  let fixture;
  try {
    const reservation = net.createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const port = reservation.address().port;
    await new Promise((resolve, reject) => reservation.close(error => error ? reject(error) : resolve()));
    socket.bind(`tcp://127.0.0.1:${port}`);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');
    // Core STREAM framing: 16-bit header length, 32-bit body length, then bytes.
    client.write(Buffer.from([0, 1, 0, 0, 0, 1, 0, 0]));
    const deadline = performance.now() + 1000;
    while (!socket.recvPacket(packet, zlink.RecvFlags.DontWait)) {
      assert(performance.now() < deadline, 'Core must receive the initial STREAM packet');
      await new Promise(resolve => setImmediate(resolve));
    }
    const routingId = packet.routingId;
    assert(routingId !== null);
    fixture = sessionFixture(undefined, socket, routingId);
    await fixture.bind();
    packet.close();
    const disconnected = once(client, 'close');
    socket.disconnectRid(routingId);
    await disconnected;
    assert.throws(() => socket.disconnectRid(routingId), error =>
      error instanceof zlink.ConfigError && error.result === zlink.ConfigResult.NotFound && error.nativeErrno === 2);
    assert.equal(await fixture.deliver(), 'application');
    fixture.session.enqueueDisconnected();
    fixture.session.enqueueDisconnected();
    await fixture.removal;
    assert.equal(await fixture.bindingRuntime.find('actor-a'), undefined);
    assert.deepEqual(fixture.session.context.actors.bound, []);
    assert.deepEqual(fixture.counts, { closed: 1, removed: 1, disconnectCalls: 1 });
  } finally {
    client?.destroy();
    await fixture?.dispose();
    packet.close(); socket.close(); context.close();
  }
});
