const assert = require('node:assert/strict');
const test = require('node:test');
const { Inject, Injectable, Module, Optional, Scope } = require('@nestjs/common');
const { NestFactory, ModuleRef, DiscoveryService } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const { createRuntimeHost } = require('../../packages/nestjs/dist/providers');

async function metadataContext(providers) {
  class MetadataModule {}
  Module({ providers: [DiscoveryService, ...providers] })(MetadataModule);
  return NestFactory.createApplicationContext(MetadataModule, { logger: false });
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
