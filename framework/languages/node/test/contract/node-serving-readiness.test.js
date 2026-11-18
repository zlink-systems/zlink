'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
require('reflect-metadata');
const { Inject, Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('@zlink-systems/framework');
const nestjs = require('@zlink-systems/nestjs');
const {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} = require('@zlink-systems/framework-locations-redis');

test('fixed RID publishes service readiness after host preparation', { timeout: 45000 }, async (t) => {
  const endpoint = process.env.ZLINK_REDIS_TEST_ENDPOINT;
  if (!endpoint) {
    t.skip('ZLINK_REDIS_TEST_ENDPOINT is required.');
    return;
  }
  const url = endpoint.startsWith('redis://') ? endpoint : `redis://${endpoint}`;
  const prefix = `node-serving-readiness:${process.pid}:${Date.now()}`;
  let factories = 0;
  let initialized = 0;
  let initializationGate;
  let initializationEntered;
  const bootstrap = {};

  class Ping {}
  class Settings {
    constructor() { factories += 1; }
    async onInitialize() {
      initialized += 1;
      initializationEntered?.();
      await initializationGate;
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(Settings);
  class Handler {
    async handle() { return { ok: true }; }
  }
  nestjs.zlinkSpotPacketHandler({ spot: () => Settings, packetName: 'Ping' })(Handler);
  const request = (outbound, id, type) => outbound.requestToSpot(id, new Ping())
    .inMesh('mesh').instanceSpot(type).timeout(5000).submit();
  const outcome = (operation) => operation.then(
    (reply) => ({ reply }),
    (error) => ({ error })
  );

  class Bootstrap {
    constructor(outbound, runtime, topology) {
      this.outbound = outbound;
      this.runtime = runtime;
      this.topology = topology;
    }
    async onApplicationBootstrap() {
      bootstrap.state = this.runtime.status.state;
      bootstrap.placement = this.topology.snapshot('mesh').placement;
      [bootstrap.explicit, bootstrap.inferred] = await Promise.all([
        outcome(request(this.outbound, 'node-settings', 'settings')),
        outcome(request(this.outbound, 'inferred-settings'))
      ]);
      bootstrap.initialized = initialized;
      bootstrap.completedState = this.runtime.status.state;
    }
  }
  Inject(nestjs.ZLINK_SPOT_OUTBOUND)(Bootstrap, undefined, 0);
  Inject(nestjs.ZLINK_FRAMEWORK_RUNTIME)(Bootstrap, undefined, 1);
  Inject(nestjs.ZLINK_ROUTE_MESH_RUNTIME)(Bootstrap, undefined, 2);
  Injectable()(Bootstrap);
  const builder = nestjs.zlinkFramework()
    .addLocationStore(new ZLinkRedisLocationStore({ url, keyPrefix: `${prefix}:location` }))
    .addRelocationStore(new ZLinkRedisRelocationStore({ url, keyPrefix: `${prefix}:blob` }));
  builder.configureDispatch().messageFlow('detailed');
  const objects = builder.addRouteMesh('mesh').listen('tcp://127.0.0.1:0')
    .routingId('fixed-node').objects();
  objects.client();
  objects.server().addInstanceSpotFactory('settings', Settings, (factory) => factory.disableRelocation());
  class App {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [Settings, Handler, Bootstrap]
  })(App);
  const app = await NestFactory.createApplicationContext(App, { logger: false, abortOnError: false });
  let release;
  try {
    await t.test('bootstrap request ends with the section 4.1 terminal', () => {
      assert.equal(bootstrap.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      if (bootstrap.explicit.error) {
        assert.equal(bootstrap.explicit.error.kind, framework.ZLinkFrameworkErrorKind.NotFound,
          bootstrap.explicit.error.stack);
      } else {
        assert.equal(bootstrap.completedState, framework.ZLinkFrameworkRuntimeState.Serving);
        assert.deepEqual(bootstrap.explicit.reply, { ok: true });
      }
    });
    await t.test('Preparing excludes the local node from inferred Instance placement', () => {
      assert.equal(bootstrap.placement.isAvailable, false);
      assert.equal(bootstrap.inferred.error?.kind, framework.ZLinkFrameworkErrorKind.NotFound,
        bootstrap.inferred.error?.stack);
      assert.equal(bootstrap.initialized, 0);
    });
    const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
    for await (const event of runtime.observe(AbortSignal.timeout(30000))) {
      if (event.status.state === framework.ZLinkFrameworkRuntimeState.Serving) break;
    }
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    const outbound = app.get(nestjs.ZLINK_SPOT_OUTBOUND);
    await t.test('the same public request succeeds after Serving', async () => {
      assert.deepEqual(await request(outbound, 'node-settings', 'settings'), { ok: true });
    });
    await t.test('concurrent requests to one target invoke its factory once', async () => {
      const before = factories;
      const beforeInitialized = initialized;
      const entered = new Promise((resolve) => { initializationEntered = resolve; });
      initializationGate = new Promise((resolve) => { release = resolve; });
      const requests = Array.from({ length: 4 }, () =>
        request(outbound, 'concurrent-settings', 'settings'));
      const completed = Promise.all(requests);
      // Initialization holds the first activation while the other public requests join it.
      await Promise.race([entered, completed]);
      release();
      assert.deepEqual(await completed, Array.from({ length: 4 }, () => ({ ok: true })));
      assert.equal(factories - before, 1);
      assert.equal(initialized - beforeInitialized, 1);
    });
  } finally {
    release?.();
    await app.close();
  }
});
