const assert = require('node:assert/strict');
const test = require('node:test');
const deferred = require('./helpers/deferred.cjs');
const { AsyncLocalStorage } = require('node:async_hooks');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const connector = require('../../packages/stream-connector/dist');
const framework = require('../../packages/framework/dist/internal');
const nest = require('../../packages/nestjs/dist');

function withTimeout(promise, timeoutMs, label) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timed out`)), timeoutMs);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

test('login admission limits two registered listeners and bypasses real Actor and Store calls', {
  timeout: 10_000
}, async (t) => {
  const requests = new AsyncLocalStorage();
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const storeWork = [];
  for (const operation of ['read', 'write', 'scan']) {
    const original = store[operation].bind(store);
    t.mock.method(store, operation, (...args) => {
      const argument = args[0];
      const keys = operation === 'read' ? [argument.value] : operation === 'write'
        ? argument.mutations.map(({ key }) => key.value) : [];
      const actorId = keys.some((key) => key.endsWith(':rejected')) ? 'rejected'
        : keys.some((key) => key.endsWith(':held')) ? 'held' : requests.getStore();
      if (actorId !== undefined) storeWork.push({ actorId, operation });
      return original(...args);
    });
  }
  const entered = deferred();
  const release = deferred();
  const waiting = deferred();
  const unexpectedDispatch = deferred();
  const created = [];
  const handlerIds = [];
  const clients = [];
  const signal = new AbortController();
  let app;
  let actors;
  class ActorFactory {
    async create(context) {
      created.push(context.actorId);
      return { context };
    }
  }
  class Entry {
    constructor(context) { this.context = context; }
    configure() {}
    async onCreateActor() { return { accepted: true }; }
  }
  class SessionFactory {
    async create(context) {
      return {
        context,
        async onDispatch(dispatch, payload) {
          if (dispatch.packetName === 'Probe') {
            await context.client.reply({ alive: true }).submit();
            return;
          }
          const { actorId } = payload.decode();
          handlerIds.push(actorId);
          if (actorId === 'rejected') unexpectedDispatch.resolve(actorId);
          await requests.run(actorId, async () => {
            await actors.getOrCreate(actorId, 'player').inMesh('login-admission').submit();
            await actors.find(actorId);
            entered.resolve();
            await release.promise;
            await context.client.reply({ actorId }).submit();
          });
        }
      };
    }
  }

  class SecondSessionFactory extends SessionFactory {}
  const builder = nest.zlinkFramework().addLocationStore(store);
  builder.addRouteMesh('login-admission').listen('tcp://127.0.0.1:0')
    .objects().server().addEntrySpot(Entry, 'player')
    .addActorFactory('player', ActorFactory, (factory) => factory.disableRelocation());
  for (const [listener, factory] of [['first', SessionFactory], ['second', SecondSessionFactory]]) {
    const stream = builder.addStreamNode(listener).bind('ws://127.0.0.1:*')
      .enableActorDispatch().registerSession(factory);
    // Without stage 1 the same host dispatches the second Login.
    stream.setAuthenticationAdmission('Login', 1, 500);
  }
  class AppModule {}
  Module({
    imports: [nest.ZLinkModule.forRoot(builder.build())],
    providers: [ActorFactory, SessionFactory, SecondSessionFactory]
  })(AppModule);
  try {
    app = await NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false });
    const runtime = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
    actors = app.get(nest.ZLINK_ACTOR_MANAGER);
    const getOrCreate = t.mock.method(actors, 'getOrCreate');
    const find = t.mock.method(actors, 'find');
    for (const listener of ['first', 'second']) {
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint: runtime.getListenerStatus('stream', listener).endpoint,
        heartbeat: { enabled: false },
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
      });
      clients.push(client);
      await client.connect();
    }
    const [first, second] = clients;
    second.onAdmissionWaiting(waiting.resolve);
    const firstResult = first.request({ actorId: 'held' }).packetName('Login')
      .submit().then((reply) => ({ reply }), (error) => ({ error }));
    await withTimeout(entered.promise, 2000, 'accepted Actor work');
    const secondResult = second.request({ actorId: 'rejected' }).packetName('Login')
      .submit(signal.signal).then((reply) => ({ reply }), (error) => ({ error }));
    const rejection = await withTimeout(Promise.race([
      waiting.promise, secondResult, unexpectedDispatch.promise
    ]), 2000, 'second listener admission');
    assert.deepEqual(rejection, { requestPacketName: 'Login', retryAfterMs: 500 });
    signal.abort();
    assert.equal((await secondResult).error.error.code, connector.ZlinkStreamErrorCode.Disconnected);
    assert.deepEqual(await second.request({}).packetName('Probe').submit(), { alive: true });
    assert.deepEqual(handlerIds, ['held']);
    assert.deepEqual(created, ['held']);
    assert.deepEqual(getOrCreate.mock.calls.map(({ arguments: [actorId] }) => actorId), ['held']);
    assert.deepEqual(find.mock.calls.map(({ arguments: [actorId] }) => actorId), ['held']);
    assert.ok(storeWork.some(({ operation }) => operation === 'read'));
    assert.ok(storeWork.some(({ operation }) => operation === 'write'));
    assert.equal(storeWork.some(({ actorId }) => actorId === 'rejected'), false);
    runtime.resetCapacityMetrics();
    release.resolve();
    assert.deepEqual(await firstResult, { reply: { actorId: 'held' } });
  } finally {
    signal.abort();
    release.resolve();
    for (const client of clients) await client.close();
    await app?.close();
  }
});
