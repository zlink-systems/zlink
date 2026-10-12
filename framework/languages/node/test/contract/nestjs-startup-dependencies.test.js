const assert = require('node:assert/strict');
const test = require('node:test');
const { Inject, Injectable, Module, Optional, Scope } = require('@nestjs/common');
const { NestFactory, ModuleRef, DiscoveryService } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const { createRuntimeHost } = require('../../packages/nestjs/dist/providers');
const {
  ZLINK_INTERNAL_APPLICATION_JOB_QUEUE_HANDLER_START_GATE: handlerStartGateToken
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-handler-start-gate');

async function metadataContext(providers) {
  class MetadataModule {}
  Module({ providers: [DiscoveryService, ...providers] })(MetadataModule);
  return NestFactory.createApplicationContext(MetadataModule, { logger: false });
}

for (const present of [false, true]) {
  test(`handler start gate is resolved at registration (${present ? 'present' : 'absent'})`, async () => {
    const gate = {
      hold: true,
      shouldHoldPermitBeforeHandler() {
        return this.hold;
      }
    };
    const context = await metadataContext(
      present ? [{ provide: handlerStartGateToken, useValue: gate }] : []
    );
    const moduleRef = context.get(ModuleRef);
    const discovery = context.get(DiscoveryService);
    const get = moduleRef.get.bind(moduleRef);
    const providers = discovery.getProviders.bind(discovery);
    let missingLookups = 0;
    let scans = 0;
    moduleRef.get = (...args) => {
      if (args[0] === handlerStartGateToken && !present) missingLookups++;
      return get(...args);
    };
    discovery.getProviders = () => {
      scans++;
      return providers();
    };
    try {
      const runtime = createRuntimeHost(
        framework.createFrameworkRegistration(),
        moduleRef,
        discovery
      );
      const registrationScans = scans;
      for (const hold of [true, false, true]) {
        gate.hold = hold;
        const permit = await runtime.applicationJobQueue.acquire(undefined, 'remote');
        permit.markApplicationQueued();
        permit.releaseBeforeHandler();
        assert.equal(
          runtime.applicationJobQueue.snapshot().permitsInUse,
          present && hold ? 1n : 0n
        );
        permit.releaseAfterInternalProcessing();
      }
      assert.equal(missingLookups, 0, 'provider absence must not invoke throwing ModuleRef.get');
      assert.equal(scans, registrationScans, 'permit release must not rediscover providers');
      await runtime.shutdown();
    } finally {
      await context.close();
    }
  });
}

function registration(type) {
  const value = framework.createFrameworkRegistration();
  value.spotFactories.add(type);
  return value;
}

test('Nest startup rejects a missing required dependency from metadata', async () => {
  class Dependency {}
  class RequiredSpot {
    constructor(dependency) {
      throw new Error('Startup must not construct a Spot');
    }
  }
  Reflect.defineMetadata('design:paramtypes', [Dependency], RequiredSpot);
  const context = await metadataContext([]);
  try {
    assert.throws(() =>
      createRuntimeHost(
        registration(RequiredSpot),
        context.get(ModuleRef),
        context.get(DiscoveryService)
      )
    );
  } finally {
    await context.close();
  }
});

test('Nest startup respects Inject tokens and Optional without constructing scoped dependencies', async () => {
  let constructed = 0;
  class Dependency {
    constructor() {
      constructed++;
    }
  }
  Injectable({ scope: Scope.REQUEST })(Dependency);
  class QualifiedSpot {
    constructor(selected, missing) {
      throw new Error('Startup must not construct a Spot');
    }
  }
  Inject('selected')(QualifiedSpot, undefined, 0);
  Inject('missing')(QualifiedSpot, undefined, 1);
  Optional()(QualifiedSpot, undefined, 1);
  const context = await metadataContext([
    { provide: 'first', useClass: Dependency, scope: Scope.REQUEST },
    { provide: 'selected', useClass: Dependency, scope: Scope.REQUEST }
  ]);
  try {
    const runtime = createRuntimeHost(
      registration(QualifiedSpot),
      context.get(ModuleRef),
      context.get(DiscoveryService)
    );
    assert.equal(constructed, 0);
    await runtime.shutdown();
  } finally {
    await context.close();
  }
});
