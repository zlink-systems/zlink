'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');

test('User Spot context Close commits and releases its authority through the host coordinator', async () => {
  const events = [];
  class RoomSpot {
    async onClosing() {
      events.push('onClosing');
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(RoomSpot);
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder
    .addRouteMesh('context-close')
    .listen('tcp://127.0.0.1:0')
    .routingId('context-close-node')
    .objects()
    .server()
    .addSpotFactory('room', RoomSpot, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [RoomSpot] })(
    AppModule
  );
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
    const created = await spots.create('room').inMesh('context-close').submit();
    const spotId = created.spot.spotId;
    assert.notEqual(await spots.find(spotId), undefined);

    let close;
    const reply = await runtime.spotManager.executeOnSpot(RoomSpot, spotId, (spot) => {
      close = spot.context.close();
      events.push('handlerReturned');
      return 'reply';
    });
    assert.equal(reply, 'reply');
    assert.equal(await close, true);
    assert.equal(await spots.find(spotId), undefined);
    assert.deepEqual(events, ['handlerReturned', 'onClosing']);
  } finally {
    await app.close();
  }
});
