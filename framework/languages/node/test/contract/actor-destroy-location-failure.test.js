const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const {
  decodeActorAuthorityPayload
} = require('../../packages/framework/dist/runtime/actors/actor-authority-payload-codec');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

async function startActorHost(t, fault) {
  const base = new ZLinkInMemoryProviderLocationStore();
  const storeError = new Error('Controlled Actor authority write failure');
  const deleteObserved = deferred();
  const allowDelete = deferred();
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
      if (
        fault === 'closing' &&
        remainingFaults > 0 &&
        request.mutations.some(
          (mutation) =>
            mutation.kind === 'put' &&
            mutation.key.value === 'authority\0actor\0destroy-player' &&
            actorAuthorityState(JSON.parse(Buffer.from(mutation.bytes).toString('utf8'))) ===
              'closing'
        )
      ) {
        remainingFaults--;
        throw storeError;
      }
      const deletion = request.mutations.find(
        (mutation) =>
          mutation.kind === 'delete' && mutation.key.value === 'authority\0actor\0destroy-player'
      );
      if (deletion !== undefined) {
        deleteCalls++;
        if (remainingFaults > 0 && fault === 'pause') {
          deleteObserved.resolve();
          await allowDelete.promise;
        }
        if (remainingFaults > 0) {
          remainingFaults--;
          if (fault === 'pause') return await base.write(request, signal);
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
      const authority = await readAuthority();
      return { state: actorAuthorityState(authority) };
    }
  }
  class ReadGeneration {
    async handle() {
      return { generation: String(actorContext.objectGeneration) };
    }
  }
  nestjs.zlinkEntrySpotActorRequestHandler({
    entrySpot: () => Entry,
    actor: () => Player,
    packetName: 'SelfDestroy'
  })(SelfDestroy);
  nestjs.zlinkEntrySpotActorRequestHandler({
    entrySpot: () => Entry,
    actor: () => Player,
    packetName: 'ReadGeneration'
  })(ReadGeneration);
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
    providers: [Factory, SelfDestroy, ReadGeneration]
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
  async function readAuthority() {
    const result = await base.read({ value: 'authority\0actor\0destroy-player' });
    assert.equal(result.kind, 'found');
    return JSON.parse(Buffer.from(result.value.bytes).toString('utf8'));
  }

  return {
    actors,
    actorRef,
    create,
    storeError,
    deleteObserved: deleteObserved.promise,
    allowDelete: allowDelete.resolve,
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
    selfDestroy: () => request('SelfDestroy'),
    readGeneration: () => request('ReadGeneration'),
    authority: readAuthority
  };

  async function request(packetName) {
    return await app
      .get(nestjs.ZLINK_ACTOR_CLIENT)
      .requestToActor(actorRef.actorId, {})
      .packetName(packetName)
      .submit();
  }
}

function actorAuthorityState(authority) {
  return decodeActorAuthorityPayload(Buffer.from(authority.payload, 'base64')).state;
}

test(
  'destroy rolls back admission and teardown when the Closing CAS fails',
  { timeout: 20000 },
  async (t) => {
    const host = await startActorHost(t, 'closing');
    host.arm();
    await assert.rejects(
      () => host.actors.destroy(host.actorRef),
      (error) =>
        error.kind === ZLinkFrameworkErrorKind.Unavailable && error.cause === host.storeError
    );
    assert.equal(actorAuthorityState(await host.authority()), 'ready');
    assert.equal(host.deleteCalls, 0);
    assert.deepEqual(await host.readGeneration(), {
      generation: String(host.actorRef.objectGeneration)
    });
    assert.equal(await host.actors.destroy(host.actorRef), true);
  }
);

for (const self of [true, false]) {
  test(
    `destroy retains the failed terminal for its exact incarnation (self=${self})`,
    { timeout: 20000 },
    async (t) => {
      const host = await startActorHost(t, 'throw');
      host.arm();
      if (self) {
        assert.deepEqual(await host.selfDestroy(), { state: 'closing' });
        await host.deleteObserved;
      }
      const error = await host.actors.destroy(host.actorRef).catch((error) => error);
      assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
      assert.equal(error.cause, host.storeError);
      await assert.rejects(
        () => host.actors.destroy(host.actorRef),
        (observed) => observed.kind === error.kind && observed.cause === host.storeError
      );
      assert.equal(await host.actors.find(host.actorRef.actorId), undefined);
      assert.equal(host.contextGeneration, host.actorRef.objectGeneration);
      assert.equal(host.deleteCalls, 1);
      const authority = await host.authority();
      assert.equal(authority.objectGeneration, String(host.actorRef.objectGeneration));
      assert.equal(actorAuthorityState(authority), 'closing');
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
    await assert.rejects(
      () => host.actors.destroy(host.actorRef),
      (error) => error.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert.equal(host.deleteCalls, 2);
    assert.equal(await host.actors.find(host.actorRef.actorId), undefined);
    const authority = await host.authority();
    assert.equal(authority.objectGeneration, String(host.actorRef.objectGeneration));
    assert.equal(actorAuthorityState(authority), 'closing');
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

for (const operation of ['find', 'create', 'join', 'self', 'stale']) {
  test(`Actor destroy lifecycle contract (${operation})`, { timeout: 20000 }, async (t) => {
    const host = await startActorHost(t, 'pause');
    const ready = await host.authority();
    host.arm();
    const destroy = operation === 'self' ? host.selfDestroy() : host.actors.destroy(host.actorRef);
    const joined = [];
    try {
      await host.deleteObserved;
      if (operation === 'find') {
        assert.equal(await host.actors.find(host.actorRef.actorId), undefined);
      } else if (operation === 'create') {
        const next = host.create();
        joined.push(next);
        const observed = await Promise.race([
          next,
          new Promise((resolve) => setTimeout(resolve, 30, 'pending'))
        ]);
        assert.equal(observed, 'pending');
      } else if (operation === 'join' || operation === 'self') {
        if (operation === 'self') assert.deepEqual(await destroy, { state: 'closing' });
        const closing = await host.authority();
        for (const field of [
          'objectGeneration',
          'authorityOwnerGeneration',
          'ownerId',
          'ownerLeaseGeneration'
        ]) {
          assert.equal(closing[field], ready[field]);
        }
        const second = host.actors.destroy(host.actorRef);
        joined.push(second);
        const observed = await Promise.race([
          second,
          new Promise((resolve) => setImmediate(resolve, 'pending'))
        ]);
        assert.equal(observed, 'pending');
      }
    } finally {
      host.allowDelete();
      await destroy;
      await Promise.allSettled(joined);
    }
    if (operation === 'join' || operation === 'self') assert.equal(await joined[0], true);
    if (operation === 'create' || operation === 'stale') {
      const next = await (joined[0] ?? host.create());
      assert.equal(next.status, 'created');
      assert.ok(next.actor.objectGeneration > host.actorRef.objectGeneration);
      if (operation === 'stale') {
        await assert.rejects(
          () => host.actors.destroy(host.actorRef),
          (error) => error.kind === ZLinkFrameworkErrorKind.InvalidOperation
        );
        assert.deepEqual(await host.actors.find(next.actor.actorId), next.actor);
      }
    }
  });
}
