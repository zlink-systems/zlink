const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

const cases = JSON.parse(
  readFileSync('../../runtime/conformance/bound-session-relocation-v1.json', 'utf8')
).sameNodeDuplicateBind;
for (const scenario of cases) {
  test(`public same-node duplicate bind: ${scenario.name}`, { timeout: 20000 }, async (t) => {
    let callbacks = 0;
    let replacements = 0;
    let ingress = 0;
    let player;
    let connected;
    const clients = [];
    class Player {
      constructor(context) {
        this.context = context;
        player = this;
      }
      async onDuplicateBind() {
        callbacks++;
        return scenario.rejectionMessage || undefined;
      }
    }
    class Factory {
      async create(context) {
        return new Player(context);
      }
    }
    class Entry {
      constructor(context) {
        this.context = context;
      }
      configure() {}
      async onJoinedActor() {}
      async onLeaveActor() {}
      async onDisconnectActor() {}
    }
    class ProbeHandler {
      async handle(_spot, _actor, _context, request) {
        ingress++;
        return request.value;
      }
    }
    nestjs.zlinkEntrySpotActorRequestHandler({
      entrySpot: () => Entry,
      actor: () => Player,
      packetName: 'Probe'
    })(ProbeHandler);
    class SessionFactory {
      async create(context) {
        const session = {
          context,
          async onConnected() {
            connected(session);
          },
          async onActorBindingReplaced() {
            replacements++;
          },
          async onDispatch(dispatch, message) {
            if (dispatch.actor) await dispatch.actor.relay(dispatch, message);
          }
        };
        return session;
      }
    }
    const builder = nestjs
      .zlinkFramework()
      .addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder.configureDispatch().messageFlow('normal');
    builder
      .addRouteMesh('binding')
      .listen('tcp://127.0.0.1:0')
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
    builder
      .addStreamNode('clients')
      .bind('ws://127.0.0.1:*')
      .enableActorDispatch()
      .registerSession(SessionFactory);
    class AppModule {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [Factory, SessionFactory, ProbeHandler]
    })(AppModule);
    const app = await createBrowserSafeStreamApp(
      () => NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false }),
      'clients'
    );
    t.after(async () => {
      for (const client of clients.reverse()) await client.close();
      await app.close();
    });
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    await actors.create('same-node-player', 'player').inMesh('binding').submit();
    const actor = await actors.find('same-node-player');
    async function open() {
      const sessionReady = new Promise((resolve) => {
        connected = resolve;
      });
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint: app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).getListenerStatus('stream', 'clients')
          .endpoint,
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
        reconnect: { enabled: false }
      });
      clients.push(client);
      let actorBound;
      const bound = new Promise((resolve) => {
        actorBound = resolve;
      });
      client.onActorBound(actorBound);
      let closed;
      const disconnected = new Promise((resolve) => {
        closed = resolve;
      });
      client.onDisconnected(closed);
      await client.connect();
      return { client, session: await sessionReady, bound, disconnected };
    }
    const current = await open();
    await current.session.context.actors.bind(actor);
    const oldActor = await current.bound;
    assert.equal(
      await oldActor.request({ value: 'before' }).packetName('Probe').submit(),
      'before'
    );
    const incoming = await open();
    if (scenario.accepted) {
      await incoming.session.context.actors.bind(actor);
      const successor = await incoming.bound;
      assert.equal(
        await successor.request({ value: 'current' }).packetName('Probe').submit(),
        'current'
      );
      let oldPushes = 0;
      let pushed;
      const delivered = new Promise((resolve) => {
        pushed = resolve;
      });
      current.client.on('Push', () => {
        oldPushes++;
      });
      incoming.client.on('Push', pushed);
      await player.context.boundSession.send({ value: 'current' }).packetName('Push').submit();
      await delivered;
      assert.equal(oldPushes, 0);
      await assert.rejects(async () =>
        oldActor.request({ value: 'retired' }).packetName('Probe').submit()
      );
      await current.disconnected;
      assert.equal(ingress, 2);
    } else {
      await assert.rejects(
        () => incoming.session.context.actors.bind(actor),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected
      );
      assert.ok(current.session.context.actors.find(actor.actorId));
      assert.equal(incoming.session.context.actors.find(actor.actorId), undefined);
      assert.equal(
        await oldActor.request({ value: 'current' }).packetName('Probe').submit(),
        'current'
      );
    }
    assert.equal(callbacks, scenario.callbackCount);
    assert.equal(replacements, scenario.replacementCount);
  });
}
