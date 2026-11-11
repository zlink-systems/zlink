const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const integration = require('../../packages/framework/dist/nest-integration');

class Ping { constructor(value = 'ping') { this.value = value; } }

function fixture(t) {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  let storeCalls = 0;
  const countedStore = new Proxy(store, {
    get(target, key) {
      const value = Reflect.get(target, key);
      return typeof value === 'function'
        ? (...args) => { storeCalls++; return value.apply(target, args); }
        : value;
    }
  });
  const registration = framework.createFrameworkRegistrationWithBuilder(builder => {
  builder.addLocationStore(countedStore);
  builder.addClientServerChannel('work').client();
  const mesh = builder.addRouteMesh('mesh').listen('tcp://127.0.0.1:*').routingId('node');
  mesh.objects().client();
  mesh.channel('events').client();
  builder.addFanoutChannel('fanout').enablePublisher('tcp://127.0.0.1:*').routingId('fanout');
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  const clients = {
    channel: integration.createIntegrationChannelClient(registration, host),
    route: integration.createIntegrationRouteClient(registration, host),
    fanout: integration.createIntegrationFanoutClient(registration, host),
    publisher: integration.createIntegrationSpotPublisherClient(registration, host),
    actor: integration.createIntegrationActorClient(host),
    spot: integration.createIntegrationSpotOutbound(host)
  };
  let transportCalls = 0;
  for (const [transport, methods] of [
    [host.routeTransport, ['submit', 'request', 'submitToChannel', 'requestToChannel', 'sendToSpot', 'requestToSpot']],
    [host.spotAddressTransport, ['sendToSpotAddress', 'requestToSpotAddress']],
    [host.spotPublisherTransport, ['publish']]
  ]) {
    for (const method of methods) {
      if (typeof transport[method] === 'function') {
        t.mock.method(transport, method, () => { transportCalls++; throw new Error('Unexpected prestart transport'); });
      }
    }
  }
  const initialStoreCalls = storeCalls;
  return { host, clients, counters: () => [storeCalls - initialStoreCalls, transportCalls] };
}

function spotHandle() {
  const target = { meshName: 'mesh', nodeRid: 'remote', spotId: 'spot', spotKind: framework.ZLinkSpotKind.User };
  return framework.createSpotHandle('spot', target, async () => target);
}

const cases = {
  'Channel send': c => c.channel.sendToChannel('work', new Ping()),
  'Channel request': c => c.channel.requestToChannel('work', new Ping()),
  'Route Channel send': c => c.route.sendToChannel('events', new Ping()),
  'Route Channel request': c => c.route.requestToChannel('events', new Ping()),
  'Node send': c => c.route.sendToNode('mesh', 'node', new Ping()),
  'Node request': c => c.route.requestToNode('mesh', 'node', new Ping()),
  'SpotHandle send': c => c.route.sendToSpot(spotHandle(), new Ping()),
  'SpotHandle request': c => c.route.requestToSpot(spotHandle(), new Ping()),
  'Spot send': c => c.spot.sendToSpot('spot', new Ping()),
  'Spot request': c => c.spot.requestToSpot('spot', new Ping()),
  'Instance send': c => c.spot.sendToSpot('spot', new Ping()).instanceSpot('Room').inMesh('mesh'),
  'Instance request': c => c.spot.requestToSpot('spot', new Ping()).instanceSpot('Room').inMesh('mesh'),
  'Actor send': c => c.actor.sendToActor('actor', new Ping()),
  'Actor request': c => c.actor.requestToActor('actor', new Ping()),
  'Logical Multicast': c => c.publisher.publish('mesh', 'events', 'topic', new Ping()),
  'Classic fanout': c => c.fanout.publish('fanout', 'topic', new Ping())
};

for (const [name, createCall] of Object.entries(cases)) {
  test(`prestart ${name} fails at terminal without Store or transport work`, async t => {
    const { host, clients, counters } = fixture(t);
    try {
      let call;
      assert.doesNotThrow(() => { call = createCall(clients); });
      assert.equal(host.isStarted, false);
      await assert.rejects(async () => call.submit(), error =>
        error instanceof framework.ZLinkFrameworkException &&
        error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation);
      assert.deepEqual(counters(), [0, 0]);
      assert.equal(host.isStarted, false);
    } finally {
      await host.stop();
    }
  });
}

test('DI call admission is fixed before startup and hosted bootstrap can use a prepared call', async () => {
  const { Module } = require('@nestjs/common');
  const { NestFactory } = require('@nestjs/core');
  const nestjs = require('../../packages/nestjs/dist');
  let earlyResult;
  let hostedReply;
  let hostedStarted;
  class PingHandler { handle(request) { return { value: request.value }; } }
  class NoticeHandler { handle() {} }
  const builder = nestjs.zlinkFramework();
  const channel = builder.addClientServerChannel('work');
  channel.client();
  channel.server().listen().addRequestHandler('Ping', PingHandler).addSendHandler('Ping', NoticeHandler);
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [PingHandler, NoticeHandler, {
      provide: 'bootstrap-probe',
      inject: [nestjs.ZLINK_CHANNEL_CLIENT, nestjs.ZLINK_FRAMEWORK_RUNTIME],
      useFactory(client, runtime) {
        const early = client.requestToChannel('work', new Ping('early'));
        const prepared = client.requestToChannel('work', new Ping('hosted'));
        assert.equal(runtime.isStarted, false);
        earlyResult = early.submit().then(value => ({ value }), error => ({ error }));
        return {
          async onApplicationBootstrap() {
            hostedStarted = runtime.isStarted;
            await client.sendToChannel('work', new Ping('notice')).submit();
            hostedReply = await prepared.submit();
          }
        };
      }
    }]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false });
  try {
    assert.equal((await earlyResult).error?.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    assert.equal(hostedStarted, true);
    assert.deepEqual(hostedReply, { value: 'hosted' });
  } finally {
    await app.close();
  }
});

test('every messaging call prepared before startup reaches its collaborator after runtime start', async t => {
  const { prepareZLinkRequestReply, identityRequestReply } = require('../../packages/framework/dist/runtime/messaging/request-result');
  const { ZLinkSubmitStatus } = require('../../packages/framework/dist/runtime/messaging/submission-result');
  const { ZLinkSpotSerialTurnExecutor } = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
  const registration = framework.createFrameworkRegistrationWithBuilder(builder => {
    builder.addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder.addClientServerChannel('work').client();
    const mesh = builder.addRouteMesh('mesh').listen('tcp://127.0.0.1:*').routingId('node');
    mesh.objects().client();
    mesh.channel('events').client();
    builder.addFanoutChannel('fanout').enablePublisher('tcp://127.0.0.1:*').routingId('fanout');
  });
  // The host owns real lifecycle admission; collaborators isolate messaging from peer discovery.
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration: framework.createFrameworkRegistration() });
  const reply = { value: 'pong' };
  let submissions = 0;
  const send = () => { submissions++; return { status: ZLinkSubmitStatus.Submitted }; };
  const request = () => { submissions++; return prepareZLinkRequestReply(Promise.resolve(reply), identityRequestReply); };
  const channelTransport = { send, request, publish: send };
  const routeTransport = { submit: send, request, submitToChannel: send, requestToChannel: request, sendToSpot: send, requestToSpot: request };
  const actorRef = { nodeRid: 'remote', actorId: 'actor', objectGeneration: 1n, meshName: 'mesh' };
  const actorRoute = {
    meshName: 'mesh', actorId: 'actor', actorType: 'Player', actorRef,
    ownerNodeRid: 'remote', ownerNodeGeneration: 7n,
    spotKind: framework.ZLinkSpotKind.Entry, spotId: 'entry', spotGeneration: 1n,
    membershipEpoch: 1n, ownerId: 'owner', ownerLeaseGeneration: 3n,
    authorityOwnerGeneration: 4n, authorityStoreVersion: 'version', updatedAt: new Date()
  };
  const clients = {
    channel: new framework.DefaultZLinkChannelClient(registration, channelTransport, routeTransport, undefined, host.admission),
    route: new framework.DefaultZLinkRouteClient(registration, routeTransport, undefined, channelTransport, host.admission),
    fanout: new framework.DefaultZLinkFanoutClient(registration, channelTransport, host.admission),
    publisher: new framework.DefaultZLinkSpotPublisherClient(registration, { publish: send }, host.admission),
    actor: new framework.DefaultZLinkActorClient({
      admission: host.admission,
      nodeProvider: () => undefined,
      completionTableProvider: () => undefined,
      locationResolver: () => ({ async resolveDirectActorRoute() { return { kind: 'ready', route: actorRoute }; } }),
      handoffCapture: () => { submissions++; return Promise.resolve(reply); }
    }),
    spot: new framework.DefaultZLinkSpotOutbound({
      admission: host.admission,
      serial: new ZLinkSpotSerialTurnExecutor(),
      addressTransport: { sendToSpotAddress: send, requestToSpotAddress: async () => { submissions++; return reply; } }
    })
  };
  const prepared = Object.entries(cases).map(([name, createCall]) => [name, createCall(clients)]);
  const rejected = Object.values(cases).map(createCall =>
    Promise.resolve(createCall(clients).submit()).then(value => ({ value }), error => ({ error })));
  assert.equal(submissions, 0);
  try {
    await host.start();
    assert.equal(host.isStarted, true);
    for (const result of await Promise.all(rejected)) {
      assert.equal(result.error?.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    }
    assert.equal(submissions, 0);
    const admission = t.mock.method(host.admission, 'requireCall');
    for (const [name, call] of prepared) {
      await t.test(name, async () => {
        const before = submissions;
        const admissions = admission.mock.callCount();
        const result = await call.submit();
        assert.equal(admission.mock.callCount(), admissions + 1);
        assert.equal(submissions, before + 1);
        if (name.toLowerCase().includes('request')) assert.deepEqual(result, reply);
      });
    }
  } finally {
    await host.stop();
  }
});
