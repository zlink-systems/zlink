'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind, ZLinkSpotCreateState } = require('../../packages/framework/dist');

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

for (const expire of [false, true]) {
  test(`GetOrCreate waits for Closing authority (deadline=${expire})`, async () => {
    let enter;
    let release;
    const entered = new Promise((resolve) => {
      enter = resolve;
    });
    const released = new Promise((resolve) => {
      release = resolve;
    });
    class Room {
      async onClosing() {
        enter();
        await released;
      }
    }
    Injectable({ scope: Scope.TRANSIENT })(Room);
    class Other {}
    Injectable({ scope: Scope.TRANSIENT })(Other);
    const {
      ZLinkInMemoryProviderLocationStore
    } = require('../../packages/framework/dist/internal');
    let observe = false;
    let observed;
    const authorityRead = new Promise((resolve) => {
      observed = resolve;
    });
    class ObservedStore extends ZLinkInMemoryProviderLocationStore {
      async read(key, signal) {
        const result = await super.read(key, signal);
        if (observe && key.value.includes('closing-room')) observed();
        return result;
      }
    }
    const builder = nestjs.zlinkFramework().addLocationStore(new ObservedStore());
    builder
      .addRouteMesh('closing-create')
      .listen('tcp://127.0.0.1:0')
      .routingId('closing-create-node')
      .objects()
      .server()
      .addSpotFactory('room', Room, (factory) => factory.disableRelocation())
      .addSpotFactory('other', Other, (factory) => factory.disableRelocation());
    class AppModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [Room, Other] })(
      AppModule
    );
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    let close;
    try {
      const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
      const original = await spots
        .getOrCreate('closing-room', 'room')
        .inMesh('closing-create')
        .submit();
      close = spots.close(original.spot);
      await entered;
      await assert.rejects(
        spots.getOrCreate('closing-room', 'other').inMesh('closing-create').submit(),
        (error) => error.kind === ZLinkFrameworkErrorKind.TypeMismatch
      );
      observe = true;
      const pending = spots
        .getOrCreate('closing-room', 'room')
        .inMesh('closing-create')
        .timeout(expire ? 100 : 5000)
        .submit();
      if (expire) {
        await assert.rejects(
          pending,
          (error) => error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
        );
      } else {
        // Attach a rejection handler before releasing the lifecycle barrier.
        const result = pending.then(
          (value) => ({ value }),
          (error) => ({ error })
        );
        await authorityRead;
        release();
        assert.equal(await close, true);
        const outcome = await result;
        if (outcome.error) throw outcome.error;
        assert.equal(outcome.value.state, ZLinkSpotCreateState.Created);
        assert.notEqual(outcome.value.spot.objectGeneration, original.spot.objectGeneration);
      }
    } finally {
      release();
      if (close) await close;
      await app.close();
    }
  });
}
