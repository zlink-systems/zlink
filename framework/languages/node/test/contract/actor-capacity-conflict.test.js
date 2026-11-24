'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

test('200 public Actor creations share capacity without Store conflicts, concurrently and sequentially', async (t) => {
  const hostClock = fixedRidHostClock(t);
  const actorCount = 200;
  let counts;
  let factories = 0;
  let callbacks = 0;
  class ActorFactory {
    async create(context) {
      factories += 1;
      return { actorId: context.actorId, context };
    }
  }
  class EntrySpot {
    constructor(context) {
      this.context = context;
    }
    async onCreateActor() {
      callbacks += 1;
      return { accepted: true };
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(EntrySpot);
  class CountingStore extends ZLinkInMemoryProviderLocationStore {
    async read(key, signal) {
      if (counts !== undefined) counts.reads += 1;
      return super.read(key, signal);
    }
    async scan(request, signal) {
      if (counts !== undefined) counts.scans += 1;
      return super.scan(request, signal);
    }
    async write(request, signal) {
      const observed = counts;
      const authority = request.mutations.some((mutation) =>
        mutation.key.value.startsWith('authority\0actor\0')
      );
      if (observed !== undefined) {
        observed.writes += 1;
        if (authority) observed.authorityWrites += 1;
      }
      const result = await super.write(request, signal);
      if (observed !== undefined && authority && result.kind === 'conflict') {
        observed.authorityConflicts += 1;
      }
      return result;
    }
  }
  const provider = new CountingStore();
  const builder = nestjs.zlinkFramework().addLocationStore(provider);
  builder.configureLocations().ownerLeaseFencingMarginMs(1);
  builder
    .addRouteMesh('capacity-conflict')
    .listen('tcp://127.0.0.1:0')
    .routingId('1304-capacity-node')
    .setActivationConcurrency(actorCount)
    .objects()
    .server()
    .addEntrySpot(EntrySpot, 'player')
    .addActorFactory('player', ActorFactory, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    await finishFixedRidHostStartup(
      app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME),
      hostClock,
      require('../../packages/framework/dist').ZLinkFrameworkRuntimeState.Preparing
    );
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    const submit = (scenario, index) =>
      actors
        .getOrCreate(`capacity-${scenario}-${index}`, 'player')
        .inMesh('capacity-conflict')
        .timeout(5000)
        .submit();
    const prefix = `zlink:v12:capacity\0capacity-conflict\0${Buffer.from('1304-capacity-node').toString('hex')}\0actors\0`;
    let active = 0;
    for (const scenario of ['concurrent', 'sequential']) {
      counts = { reads: 0, scans: 0, writes: 0, authorityWrites: 0, authorityConflicts: 0 };
      const results = [];
      if (scenario === 'concurrent') {
        results.push(
          ...(await Promise.all(
            Array.from({ length: actorCount }, (_, index) => submit(scenario, index))
          ))
        );
      } else {
        for (let index = 0; index < actorCount; index++)
          results.push(await submit(scenario, index));
      }
      const observed = counts;
      counts = undefined;
      t.diagnostic(JSON.stringify({ scenario, ...observed }));
      active += actorCount;
      for (const result of results) assert.equal(result.status, 'created');
      assert.equal(factories, active);
      assert.equal(callbacks, active);
      assert.equal(observed.authorityWrites, actorCount * 2);
      assert.equal(observed.authorityConflicts, 0);
      for (const [state, expected] of [
        ['active', active],
        ['pending', 0]
      ]) {
        const row = await provider.read({ value: prefix + state });
        assert.equal(row.kind, 'found');
        assert.equal(Buffer.from(row.value.bytes).toString(), String(expected));
      }
    }
  } finally {
    await app.close();
  }
});
