const { observeZLinkRequestReply, ZLinkRequestResult, identityRequestReply } = require('../../packages/framework/dist/runtime/messaging/request-result');
const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate } = require('node:timers/promises');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  submitBindingRequest
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');

function deferred() {
  let resolve;
  const promise = new Promise((yes) => {
    resolve = yes;
  });
  return { promise, resolve };
}

// Hold the binding reply separately so no successful-reply fallback can satisfy
// the admission assertion. The public wrappers must reach the binding consumer.
for (const path of [
  'node',
  'Channel',
  'routed Spot',
  'Spot outbound routed',
  'Spot address',
  'Spot outbound Channel',
  'Actor remote'
]) {
  test(`${path} public request observes binding admission while reply is withheld`, async () => {
    const admission = deferred();
    const reply = deferred();
    const submitted = deferred();
    const start = (onAdmitted) => {
      const pending = submitBindingRequest(
        {
          message() {
            return this;
          },
          submit: () => ({
            result: zlink.SubmitResult.Backpressured,
            admitted: admission.promise,
            reply: reply.promise
          })
        },
        [],
        undefined,
        undefined,
        onAdmitted
      );
      submitted.resolve();
      return pending;
    };
    const transport = {
      async sendToSpot() {},
      request: (entry, _mesh, _rid, _packet, _request, _timeout, _signal, _metadata, onAdmitted) =>
        observeZLinkRequestReply(start(onAdmitted), () => 'reply', identityRequestReply, entry),
      requestToChannel: (entry, _mesh, _channel, _packet, _request, _timeout, _signal, _metadata, onAdmitted) =>
        observeZLinkRequestReply(start(onAdmitted), () => 'reply', identityRequestReply, entry),
      requestToSpot: (_target, _request, options) =>
        observeZLinkRequestReply(start(options.onAdmitted), () => 'reply', identityRequestReply, new ZLinkRequestResult()),
      requestRawToSpot: (_target, _request, options) => start(options.onAdmitted)
    };
    const registration = framework.createFrameworkRegistration({
      spotNodes: {
        mesh: {
          router: { bind: 'inproc://admission-paths' },
          meshChannels: { api: { client: true } }
        }
      }
    });
    const routeClient = new framework.DefaultZLinkRouteClient(registration, transport);
    const channelClient = new framework.DefaultZLinkChannelClient(
      registration,
      undefined,
      transport
    );
    const target = {
      meshName: 'mesh',
      nodeRid: 'target',
      spotId: 'room',
      spotKind: framework.ZLinkSpotKind.User,
      spotGeneration: 1n
    };
    const handle = framework.createSpotHandle('room', async () => target);
    const addressTransport = new framework.ZLinkHostSpotAddressTransport({
      resolver: () => ({
        resolve: async () => ({ ...target, routerChannelId: 'mesh', targetNodeRid: 'target' })
      }),
      routed: transport,
      meshNames: () => ['mesh'],
      defaultRequestTimeoutMs: 1000
    });
    const outbound = new framework.DefaultZLinkSpotOutbound({
      serial: new framework.ZLinkSpotSerialTurnExecutor(),
      meshName: 'mesh',
      channelClient,
      routedTransport: transport,
      addressTransport
    });
    class Query {}
    let call;
    if (path === 'node') call = routeClient.requestToNode('mesh', 'target', new Query());
    else if (path === 'Channel') call = channelClient.requestToChannel('api', new Query());
    else if (path === 'routed Spot') call = routeClient.requestToSpot(handle, new Query());
    else if (path === 'Spot outbound routed') call = outbound.requestToSpot(handle, new Query());
    else if (path === 'Spot address') call = outbound.requestToSpot('room', new Query());
    else if (path === 'Spot outbound Channel') call = outbound.requestToChannel('api', new Query());
    else {
      const actor = { meshName: 'mesh', actorId: 'actor', nodeRid: 'target', objectGeneration: 1n };
      const actorClient = new framework.DefaultZLinkActorClient({
        nodeProvider: () => ({ status: () => ({ routingId: 'source' }) }),
        locationResolver: () => ({
          resolveDirectActorRoute: async () => ({
            kind: 'ready',
            route: {
              meshName: 'mesh',
              actorId: 'actor',
              actorRef: actor,
              ownerNodeRid: 'target',
              ownerNodeGeneration: 1n,
              spotKind: framework.ZLinkSpotKind.Entry,
              spotId: 'entry',
              spotGeneration: 1n,
              authorityOwnerGeneration: 1n,
              ownerId: 'target-owner',
              ownerLeaseGeneration: 1n
            }
          })
        }),
        routeTransport: transport
      });
      call = actorClient.requestToActor('actor', new Query());
    }
    const pending = call.submit();
    let admitted = false;
    let replied = false;
    void pending.admitted.then(() => {
      admitted = true;
    });
    const terminal = pending.then((value) => {
      replied = true;
      return value;
    });
    try {
      await submitted.promise;
      await setImmediate();
      assert.equal(admitted, false);
      assert.equal(replied, false);
      admission.resolve();
      await setImmediate();
      assert.equal(admitted, true);
      assert.equal(replied, false);
    } finally {
      admission.resolve();
      reply.resolve(
        path === 'Actor remote'
          ? [ZLinkBufferMessage.from(Buffer.from(JSON.stringify({ ok: true, response: 'reply' })))]
          : []
      );
      assert.equal(await terminal, 'reply');
    }
  });
}

test('Instance activation public request observes existing remote submission admission before completion', async () => {
  const admission = deferred();
  const reply = deferred();
  const submitted = deferred();
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      requestService(_target, _parts, _timeout, onAdmitted) {
        void admission.promise.then(onAdmitted);
        submitted.resolve();
        return reply.promise;
      }
    },
    'source',
    1n
  );
  let operation;
  const node = {
    instanceSpotPlacementTypes: () => ['Room'],
    selectObjectPlacement: () => ({
      kind: 'selected',
      target: {
        targetNodeRid: 'target',
        targetNodeGeneration: 1n,
        descriptorVersion: '1'
      }
    }),
    requestToMissingInstanceSpot(target, parts, deadline, _source, _metadata, onAdmitted) {
      operation = runtime.requestToMissingInstanceSpotFrame(target, Buffer.from('{}'), deadline, undefined, undefined, onAdmitted);
      return operation.id;
    }
  };
  const transport = new framework.ZLinkHostSpotAddressTransport({
    resolver: () => ({ resolve: async () => undefined }),
    routed: {},
    meshNames: () => ['mesh'],
    meshNode: () => node,
    completions: () => ({
      submit(start) {
        start();
        return new ZLinkRequestResult().observe(operation.promise);
      }
    }),
    defaultRequestTimeoutMs: 1000
  });
  const outbound = new framework.DefaultZLinkSpotOutbound({
    serial: new framework.ZLinkSpotSerialTurnExecutor(),
    addressTransport: transport
  });
  class Query {}
  const pending = outbound.requestToSpot('room', new Query()).instanceSpot('Room').submit();
  let admitted = false;
  let replied = false;
  void pending.admitted.then(() => {
    admitted = true;
  });
  const terminal = assert.rejects(pending, { name: 'OperationCancelledError' });
  void pending.then(
    () => {
      replied = true;
    },
    () => undefined
  );
  try {
    await submitted.promise;
    await setImmediate();
    assert.equal(admitted, false);
    admission.resolve();
    await setImmediate();
    assert.equal(admitted, true);
    assert.equal(replied, false);
  } finally {
    runtime.close();
    reply.resolve([]);
    await terminal;
  }
});


test('public routed local Spot request observes serial queue acceptance before handler reply', async () => {
  const { ZLinkRoutedSpotPacketDispatch } = require('../../packages/framework/dist/runtime/spots/spot-routed-spot-packet-dispatch');
  const reply = deferred();
  const entered = deferred();
  class Query {}
  class Handler { async handle() { entered.resolve(); return reply.promise; } }
  const activation = {
    spotId: 'room', meshName: 'mesh', spot: {},
    serial: new framework.ZLinkSpotSerialTurnExecutor(),
    handlers: { snapshot: () => [{ kind: 'packet', packetName: 'Query', handlerType: Handler }] }
  };
  const dispatcher = new ZLinkRoutedSpotPacketDispatch({ resolveActivation: () => activation });
  const client = new framework.DefaultZLinkRouteClient(
    framework.createFrameworkRegistration({ spotNodes: { mesh: { router: { bind: 'inproc://local-stage' } } } }),
    { async sendToSpot() {}, requestToSpot(_target, request, options) {
      return observeZLinkRequestReply(dispatcher.request('room', 'Query', request, { channelName: 'mesh', onAdmitted: options.onAdmitted }), (reply) => reply, identityRequestReply, new ZLinkRequestResult());
    } }
  );
  const handle = framework.createSpotHandle('room', async () => ({
    meshName: 'mesh', nodeRid: 'local', spotId: 'room', spotKind: framework.ZLinkSpotKind.User
  }));
  const pending = client.requestToSpot(handle, new Query()).submit();
  let admitted = false;
  let replied = false;
  void pending.admitted.then(() => { admitted = true; });
  void pending.then(() => { replied = true; });
  try {
    await entered.promise;
    await setImmediate();
    assert.equal(admitted, true);
    assert.equal(replied, false);
  } finally {
    reply.resolve('reply');
    assert.equal(await pending, 'reply');
  }
});
