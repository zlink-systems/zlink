'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

test('64 concurrent Actors rebuild shared capacity without repeating factory or callback', async () => {
  const actorCount = 64;
  let factories = 0;
  let callbacks = 0;
  class ActorFactory {
    async create(context) {
      factories += 1;
      return { actorId: context.actorId, context };
    }
  }
  class EntrySpot {
    constructor(context) { this.context = context; }
    async onCreateActor() {
      callbacks += 1;
      return { accepted: true };
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(EntrySpot);
  const provider = new ZLinkInMemoryProviderLocationStore();
  const builder = nestjs.zlinkFramework().addLocationStore(provider);
  builder.addRouteMesh('capacity-conflict')
    .listen('tcp://127.0.0.1:0')
    .routingId('1304-capacity-node')
    .objects().server()
    .addEntrySpot(EntrySpot)
    .addActorFactory('player', ActorFactory, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false, abortOnError: false
  });
  try {
    const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
    const results = await Promise.all(Array.from({ length: actorCount }, (_, index) =>
      actors.getOrCreate(`capacity-${index}`, 'player')
        .inMesh('capacity-conflict').timeout(5000).submit()));
    for (const result of results) assert.equal(result.status, 'created');
    assert.equal(factories, actorCount);
    assert.equal(callbacks, actorCount);
  } finally {
    await app.close();
  }
});
