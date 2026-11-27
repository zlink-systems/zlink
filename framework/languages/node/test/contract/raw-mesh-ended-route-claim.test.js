const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkRoutePacketDispatcher
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { fixture } = require('./fixtures/raw-mesh-close');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  ReadyDomain
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

const rid = 'peer';
const first = 11n;
const replacement = 12n;
const command = wire.M6aServiceWireCommand;

function setup(t) {
  const f = fixture();
  t.after(() => f.backend.close());
  const descriptor = {
    ...f.runtime.topology.localDescriptor(),
    nodeRoutingId: rid,
    advertisedEndpoint: 'tcp://127.0.0.2:28741'
  };
  const delivered = [];
  const dispatcher = new ZLinkRoutePacketDispatcher({
    routerChannelId: 'alpha',
    dispatchErrors: new ZLinkDispatchErrorReporter(),
    handlers: [
      {
        kind: 'send',
        packetName: 'Notice',
        handler: { handle: (payload) => delivered.push(payload.value) }
      }
    ]
  });

  const select = (generation) =>
    f.setRoutes(generation === undefined ? [] : [{ routingId: rid, routeGeneration: generation }]);
  const receive = async (routeGeneration, ...parts) => {
    f.records.push({
      sourceRid: rid,
      sourceRoute: Buffer.from(rid),
      routeGeneration,
      parts,
      close() {}
    });
    const result = await f.runtime.pumpOne();
    assert.equal(f.records.length, 0, 'the Framework consumed the Core receipt');
    return result;
  };
  const handshake = (kind, generation, peer = descriptor) =>
    receive(
      generation,
      kind === command.reject ? wire.encodeReject(1) : wire.encodeRouteMeshAdmission(kind, peer)
    );
  const send = (generation, value) => {
    const envelope = encodeChannelEnvelopeParts(
      3,
      'alpha',
      'Notice',
      { value },
      undefined,
      undefined,
      undefined,
      undefined,
      false
    );
    return receive(
      generation,
      wire.encodeChannelSendHeader('alpha'),
      wire.encodeMultipartApplicationPayload(
        envelope.map((part) => Buffer.from(part)),
        SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
        SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
      )
    );
  };
  const dispatch = async () => {
    const ready = f.backend.createReadyBatch(8);
    try {
      f.backend.drainReady(ReadyDomain.Application, ready);
      for (let i = 0; i < ready.records.length; i++) {
        const claim = ready.takeClaim(i);
        try {
          const batch = claim.recvBatch(f.backend.createReceiveBatch(8, 32));
          for (const record of batch.records) {
            try {
              await dispatcher.dispatchMesh(record);
            } finally {
              record.parts.forEach((part) => part.close());
              record.onTerminalCompletion?.();
            }
          }
        } finally {
          claim.release();
        }
      }
    } finally {
      ready.close();
    }
  };
  const admitFirst = async () => {
    select(first);
    await f.runtime.observeSelectedRoutes();
    assert.equal(await handshake(command.hello, first), 'infrastructure');
    assert.equal(await handshake(command.admit, first), 'infrastructure');
    assert.equal(f.runtime.isPeerRouteReady(rid), true);
  };
  const endFirst = async () => {
    select(undefined);
    await f.runtime.observeSelectedRoutes();
    assert.equal(f.runtime.isPeerRouteReady(rid), false);
  };
  return Object.assign(f, {
    descriptor,
    delivered,
    select,
    receive,
    handshake,
    send,
    dispatch,
    admitFirst,
    endFirst
  });
}

test('ChannelSend reaches its handler once after the pump observes route termination', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  assert.equal(await f.send(first, 'current'), 'application');
  await f.dispatch();
  assert.deepEqual(f.delivered, ['current']);

  await f.endFirst();
  const result = await f.send(first, 'ended');
  await f.dispatch();
  assert.deepEqual(f.delivered, ['current', 'ended'], `ended-route receipt was ${result}`);
  assert.equal(f.runtime.mailbox.pendingMessages('application'), 0);
});

test('replacement HELLO cannot receive DATA or retire the ended admission before ADMIT', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  await f.endFirst();
  const sent = [];
  f.onSubmit = (target, parts) => {
    assert.equal(target, rid);
    sent.push(wire.decodeHeader(parts[0]).command);
  };
  f.select(replacement);
  assert.equal(await f.handshake(command.hello, replacement), 'infrastructure');
  assert.deepEqual(sent, [command.hello, command.admit]);
  assert.equal(await f.send(replacement, 'early'), 'dropped');
  assert.equal(await f.send(first, 'ended'), 'application');
  assert.equal(await f.handshake(command.admit, replacement), 'infrastructure');
  assert.equal(await f.send(first, 'superseded'), 'dropped');
  assert.equal(await f.send(replacement, 'replacement'), 'application');
  await f.dispatch();
  await f.dispatch();
  assert.deepEqual(f.delivered, ['ended', 'replacement']);
  assert.equal(f.runtime.mailbox.pendingMessages('application'), 0);
});

test('ended-route handshake cannot change selected-route admission', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  await f.endFirst();
  f.select(replacement);
  await f.runtime.observeSelectedRoutes();
  for (const kind of [command.hello, command.admit, command.reject]) {
    assert.equal(await f.handshake(kind, first), 'dropped', `command ${kind}`);
    assert.equal(f.runtime.isPeerRouteReady(rid), false);
  }
  assert.equal(await f.handshake(command.update, first), 'dropped');
  assert.equal(await f.handshake(command.admit, replacement), 'infrastructure');
  assert.equal(f.runtime.isPeerRouteReady(rid), true);
});

test('descriptor withdrawal retires only the matching ended lifecycle', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  await f.endFirst();
  const { lifecycleGeneration } = f.descriptor;
  f.runtime.forgetPeerExpectation(rid, lifecycleGeneration + 1n);
  assert.equal(await f.send(first, 'retained'), 'application');
  f.runtime.forgetPeerExpectation(rid, lifecycleGeneration);
  assert.equal(await f.send(first, 'withdrawn'), 'dropped');
  await f.dispatch();
  assert.deepEqual(f.delivered, ['retained']);
});

test('a replacement descriptor retires an ended admission of the previous lifecycle', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  await f.endFirst();
  f.select(replacement);
  assert.equal(
    await f.handshake(command.hello, replacement, {
      ...f.descriptor,
      lifecycleGeneration: f.descriptor.lifecycleGeneration + 1n
    }),
    'infrastructure'
  );
  assert.equal(await f.send(first, 'previous-lifecycle'), 'dropped');
});

test('explicit peer removal retires its admission after the selected route disappears', async (t) => {
  const f = setup(t);
  await f.admitFirst();
  await f.endFirst();
  assert.equal(await f.send(first, 'retained'), 'application');
  f.backend.disconnectPeer(rid, f.descriptor.lifecycleGeneration);
  assert.equal(await f.send(first, 'removed'), 'dropped');
  await f.dispatch();
  assert.deepEqual(f.delivered, ['retained']);
});
