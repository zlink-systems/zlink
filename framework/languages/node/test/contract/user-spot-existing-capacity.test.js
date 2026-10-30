'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind, ZLinkSpotCreateState } = require('../../packages/framework/dist');

async function capacityOne(run) {
  let factoryCalls = 0;
  class Room {
    constructor() {
      factoryCalls++;
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(Room);
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder
    .addRouteMesh('existing-capacity')
    .listen('tcp://127.0.0.1:0')
    .routingId('existing-capacity-node')
    .setSpotLimit(1)
    .objects()
    .server()
    .addSpotFactory('room', Room, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [Room] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
    const original = await spots.getOrCreate('room-1', 'room').inMesh('existing-capacity').submit();
    assert.equal(original.state, ZLinkSpotCreateState.Created);
    await run(spots, original);
    assert.equal(factoryCalls, 1);
  } finally {
    await app.close();
  }
}

test('GetOrCreate returns the same Ready SpotRef when its target capacity is full', async () => {
  await capacityOne(async (spots, original) => {
    const existing = await spots.getOrCreate('room-1', 'room').inMesh('existing-capacity').submit();
    assert.equal(existing.state, ZLinkSpotCreateState.Existing);
    assert.deepEqual(existing.spot, original.spot);
  });
});

test('GetOrCreate still rejects another Spot ID when its target capacity is full', async () => {
  await capacityOne(async (spots) => {
    await assert.rejects(
      spots.getOrCreate('room-2', 'room').inMesh('existing-capacity').submit(),
      (error) =>
        error.kind === ZLinkFrameworkErrorKind.Unavailable &&
        error.message.includes('No eligible User Spot placement target is ready.')
    );
  });
});
