const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const fixture = require('../../../../runtime/conformance/actor-request-lookup-deadline-v1.json');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

test(
  'Actor request deadline includes pending authority read and prevents late submission',
  { timeout: 10000 },
  async (t) => {
    const base = new ZLinkInMemoryProviderLocationStore();
    const entered = deferred();
    const release = deferred();
    const returned = deferred();
    t.after(() => release.resolve());
    let hold = false;
    let handled = 0;
    const store = {
      async read(key, signal) {
        if (hold && key.value === 'authority\0actor\0deadline-player') {
          entered.resolve();
          await release.promise;
          try {
            return await base.read(key, signal);
          } finally {
            returned.resolve();
          }
        }
        return base.read(key, signal);
      },
      write: (request, signal) => base.write(request, signal),
      scan: (request, signal) => base.scan(request, signal),
      dispose: () => base.dispose?.()
    };
    class Player {
      constructor(context) {
        this.context = context;
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
    }
    class Ping {
      async handle() {
        handled++;
        return { value: 'pong' };
      }
    }
    nestjs.zlinkEntrySpotActorRequestHandler({
      entrySpot: () => Entry,
      actor: () => Player,
      packetName: 'Ping'
    })(Ping);
    const builder = nestjs.zlinkFramework().addLocationStore(store);
    builder.configureLocations().routeCacheMaxAgeMs(0);
    builder
      .addRouteMesh('deadline')
      .listen('tcp://127.0.0.1:0')
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
    class AppModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [Factory, Ping] })(
      AppModule
    );
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    t.after(() => app.close());
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    await actors.getOrCreate('deadline-player', 'player').inMesh('deadline').submit();
    const client = app.get(nestjs.ZLINK_ACTOR_CLIENT);
    const request = (timeout) =>
      client.requestToActor('deadline-player', {}).packetName('Ping').timeout(timeout).submit();
    assert.deepEqual(await request(3000), { value: 'pong' });
    handled = 0;
    hold = true;
    const call = request(fixture.timeoutMs);
    const terminal = call.then(
      (value) => ({ value }),
      (error) => ({ error })
    );
    try {
      await entered.promise;
      const outcome = await Promise.race([
        terminal,
        new Promise((_, reject) =>
          setTimeout(
            () => reject(new Error('request remained pending during Store read')),
            fixture.observationMs
          )
        )
      ]);
      assert.equal(outcome.error?.kind, ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(handled, fixture.lateHandlerCalls);
    } finally {
      release.resolve();
    }
    await returned.promise;
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(handled, fixture.lateHandlerCalls);
    hold = false;
    assert.deepEqual(await request(3000), { value: 'pong' });
    assert.equal(handled, 1);
  }
);
