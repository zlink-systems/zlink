const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

async function startActorHost(t, fault) {
  const base = new ZLinkInMemoryProviderLocationStore();
  const storeError = new Error('Controlled Location Store delete failure');
  const deleteObserved = deferred();
  let remainingFaults = 0;
  let deleteCalls = 0;
  let conflicts = 0;
  let actors;
  let actorRef;
  let actorContext;
  const store = {
    read: (key, signal) => base.read(key, signal),
    scan: (request, signal) => base.scan(request, signal),
    async write(request, signal) {
      const deletion = request.mutations.find(
        (mutation) =>
          mutation.kind === 'delete' && mutation.key.value === 'authority\0actor\0destroy-player'
      );
      if (deletion !== undefined) {
        deleteCalls++;
        if (remainingFaults > 0) {
          remainingFaults--;
          if (fault === 'throw') {
            deleteObserved.resolve();
            throw storeError;
          }
          const previous = await base.read(deletion.key, signal);
          assert.equal(previous.kind, 'found');
          const preserved = await base.write(
            {
              conditions: [
                { kind: 'version', key: deletion.key, expected: previous.value.version }
              ],
              mutations: [{ kind: 'put', key: deletion.key, bytes: previous.value.bytes }]
            },
            signal
          );
          assert.equal(preserved.kind, 'applied');
          const result = await base.write(request, signal);
          assert.equal(result.kind, 'conflict');
          conflicts++;
          if (remainingFaults === 0) deleteObserved.resolve();
          return result;
        }
      }
      return await base.write(request, signal);
    },
    dispose: () => base.dispose?.()
  };
  class Player {
    constructor(context) {
      this.context = context;
      actorContext = context;
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
  class SelfDestroy {
    async handle() {
      assert.equal(await actors.destroy(actorRef), true);
      return { started: true };
    }
  }
  nestjs.zlinkEntrySpotActorRequestHandler({
    entrySpot: () => Entry,
    actor: () => Player,
    packetName: 'SelfDestroy'
  })(SelfDestroy);
  const builder = nestjs.zlinkFramework().addLocationStore(store);
  builder
    .addRouteMesh('destroy')
    .listen('tcp://127.0.0.1:0')
    .objects()
    .server()
    .addEntrySpot(Entry, 'player')
    .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [Factory, SelfDestroy]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  t.after(() => app.close());
  actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
  const create = () => actors.getOrCreate('destroy-player', 'player').inMesh('destroy').submit();
  const warmup = await create();
  assert.equal(await actors.destroy(warmup.actor), true);
  const created = await create();
  actorRef = created.actor;
  assert.ok(actorRef.objectGeneration > 1n);
  deleteCalls = 0;
  return {
    actors,
    actorRef,
    create,
    storeError,
    deleteObserved: deleteObserved.promise,
    get deleteCalls() {
      return deleteCalls;
    },
    get conflicts() {
      return conflicts;
    },
    get contextGeneration() {
      return actorContext.objectGeneration;
    },
    arm() {
      remainingFaults = fault === 'conflict' ? 2 : 1;
    },
    async selfDestroy() {
      assert.deepEqual(
        await app
          .get(nestjs.ZLINK_ACTOR_CLIENT)
          .requestToActor(actorRef.actorId, {})
          .packetName('SelfDestroy')
          .submit(),
        { started: true }
      );
    },
    async authority() {
      const result = await base.read({ value: 'authority\0actor\0destroy-player' });
      assert.equal(result.kind, 'found');
      return JSON.parse(Buffer.from(result.value.bytes).toString('utf8'));
    }
  };
}

for (const self of [true, false]) {
  test(
    `destroy retries Store cleanup with its exact incarnation (self=${self})`,
    { timeout: 20000 },
    async (t) => {
      const host = await startActorHost(t, 'throw');
      host.arm();
      if (self) {
        await host.selfDestroy();
        await host.deleteObserved;
        // Let the detached terminal settle after the Store rejects its write.
        await new Promise((resolve) => setImmediate(resolve));
      } else {
        await assert.rejects(
          () => host.actors.destroy(host.actorRef),
          (error) =>
            error.kind === ZLinkFrameworkErrorKind.Unavailable && error.cause === host.storeError
        );
      }
      assert.deepEqual(await host.actors.find(host.actorRef.actorId), host.actorRef);
      assert.equal(host.contextGeneration, host.actorRef.objectGeneration);
      assert.equal(host.deleteCalls, 1);
      assert.equal(
        (await host.authority()).objectGeneration,
        String(host.actorRef.objectGeneration)
      );
      assert.equal(await host.actors.destroy(host.actorRef), true);
      assert.equal(host.deleteCalls, 2);
      assert.equal(await host.actors.find(host.actorRef.actorId), undefined);
      const next = await host.create();
      assert.equal(next.status, 'created');
      assert.ok(next.actor.objectGeneration > host.actorRef.objectGeneration);
      assert.equal(await host.actors.destroy(next.actor), true);
    }
  );
}

test(
  'destroy fails when both authority delete CAS attempts conflict',
  { timeout: 20000 },
  async (t) => {
    const host = await startActorHost(t, 'conflict');
    host.arm();
    await assert.rejects(
      () => host.actors.destroy(host.actorRef),
      (error) => error.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert.equal(host.conflicts, 2);
    assert.equal(host.deleteCalls, 2);
    assert.deepEqual(await host.actors.find(host.actorRef.actorId), host.actorRef);
    assert.equal((await host.authority()).objectGeneration, String(host.actorRef.objectGeneration));
    assert.equal(await host.actors.destroy(host.actorRef), true);
    assert.equal(host.deleteCalls, 3);
    assert.equal(await host.actors.find(host.actorRef.actorId), undefined);
    const next = await host.create();
    assert.equal(next.status, 'created');
    assert.ok(next.actor.objectGeneration > host.actorRef.objectGeneration);
    assert.equal(await host.actors.destroy(next.actor), true);
  }
);

test(
  'successful destroy allows getOrCreate to allocate a new generation',
  { timeout: 20000 },
  async (t) => {
    const host = await startActorHost(t, 'none');
    assert.equal(await host.actors.destroy(host.actorRef), true);
    const next = await host.create();
    assert.equal(next.status, 'created');
    assert.ok(next.actor.objectGeneration > host.actorRef.objectGeneration);
    assert.deepEqual(await host.actors.find(next.actor.actorId), next.actor);
    assert.equal(await host.actors.destroy(next.actor), true);
  }
);
