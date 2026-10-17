'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('@zlink-systems/nestjs');

for (const callback of ['onCreate', 'onInitialize']) {
  test(`User Spot preserves a message-less ${callback} failure through the public manager`, async () => {
    const original = new Error();
    class FailingSpot {
      onCreate() {
        if (callback === 'onCreate') throw original;
        return Promise.resolve({ accepted: true });
      }

      async onInitialize() {
        if (callback === 'onInitialize') throw original;
      }
    }
    Injectable({ scope: Scope.TRANSIENT })(FailingSpot);
    const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
    builder
      .addRouteMesh('activation-failure')
      .listen('tcp://127.0.0.1:0')
      .routingId('activation-failure-node')
      .objects()
      .server()
      .addSpotFactory('failing-spot', FailingSpot, (factory) => factory.disableRelocation());
    class AppModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [FailingSpot] })(
      AppModule
    );
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    try {
      const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
      const spotId = `activation-failure-${callback}`;
      await assert.rejects(
        spots
          .getOrCreate(spotId, 'failing-spot')
          .inMesh('activation-failure')
          .request('activation-request')
          .submit(),
        (failure) => {
          assert.strictEqual(failure, original);
          assert.equal(failure.message, '');
          return true;
        }
      );
      assert.equal(await spots.find(spotId), undefined);
    } finally {
      await app.close();
    }
  });
}
