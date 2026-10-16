const assert = require('node:assert/strict');
const test = require('node:test');
const { Inject, Injectable, Module, Scope } = require('@nestjs/common');
const { DiscoveryService, ModuleRef, NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const { ZLINK_SPOT_MANAGER } = require('../../packages/nestjs/dist');
const { createRuntimeHost } = require('../../packages/nestjs/dist/providers');

for (const scope of [Scope.TRANSIENT, Scope.REQUEST, Scope.DEFAULT]) {
  test(`Entry Spot receives constructor dependencies per activation with Nest scope ${scope}`, async () => {
    const requests = [];
    const manager = {
      getOrCreate(id) {
        requests.push(id);
        return { accepted: true };
      }
    };
    class EntrySpot {
      constructor(spots) {
        this.spots = spots;
      }
      async onCreateActor(actor) {
        return this.spots.getOrCreate(actor.actorId);
      }
    }
    Injectable({ scope })(EntrySpot);
    Inject(ZLINK_SPOT_MANAGER)(EntrySpot, undefined, 0);
    class AppModule {}
    Module({
      providers: [DiscoveryService, EntrySpot, { provide: ZLINK_SPOT_MANAGER, useValue: manager }]
    })(AppModule);
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    const registration = framework.createFrameworkRegistration();
    registration.spotFactories.add(EntrySpot);
    const runtime = createRuntimeHost(registration, app.get(ModuleRef), app.get(DiscoveryService));
    const activations = [];
    try {
      for (const actorId of ['first', 'second']) {
        const activation = new framework.ZLinkEntrySpotActivation({
          entrySpotType: EntrySpot,
          providerResolver: runtime.options.providerResolver,
          nativeSpot: { routingId: actorId, async dispose() {} },
          nodeRid: 'entry-node',
          spotNodeName: 'entry-node',
          detachedTaskRunner: runtime.detachedTaskRunner()
        });
        activations.push(activation);
        await activation.create();
        assert.deepEqual(await activation.notifyCreateActor({ actorId }, {}), { accepted: true });
        assert.equal(activation.entrySpot.spots, manager);
      }
      assert.deepEqual(requests, ['first', 'second']);
      assert.notEqual(activations[0].entrySpot, activations[1].entrySpot);
      assert.notEqual(activations[0].entrySpot.context, activations[1].entrySpot.context);
    } finally {
      for (const activation of activations) await activation.dispose();
      await runtime.shutdown();
      await app.close();
    }
  });
}
