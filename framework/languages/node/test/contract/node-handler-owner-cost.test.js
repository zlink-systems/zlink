const assert = require('node:assert/strict');
const test = require('node:test');
require('reflect-metadata');
const { Scope } = require('@nestjs/common');
const { PARAMTYPES_METADATA, PROPERTY_DEPS_METADATA } = require('@nestjs/common/constants');
const {
  prepareNestHandlerFactory,
  createRuntimeHost
} = require('../../packages/nestjs/dist/providers');
const { createHook } = require('node:async_hooks');
const bridge = require('../../packages/nestjs/dist/framework-loader').framework;
const {
  zlinkMessageMetadata,
  ZLinkMessageMetadataEmpty
} = require('../../packages/framework/dist/contracts/Common/ZLinkMessageMetadata');
const {
  runInHandlerInstanceScope
} = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');

test('default synchronous handler activation retains one public resolve Promise', async () => {
  class Handler {}
  await runInHandlerInstanceScope(undefined, undefined, async (scope) => {
    let promises = 0;
    const hook = createHook({
      init(_id, type) {
        if (type === 'PROMISE') promises++;
      }
    });
    hook.enable();
    const instance = scope.resolve(Handler);
    hook.disable();
    assert.equal(promises, 1);
    assert.equal(scope.resolve(Handler), instance);
    assert.ok((await instance) instanceof Handler);
  });
});

test('empty ingress metadata reuses the immutable empty owner while values remain snapshots', () => {
  assert.equal(zlinkMessageMetadata({}), ZLinkMessageMetadataEmpty);
  assert.equal(zlinkMessageMetadata(new Map()), ZLinkMessageMetadataEmpty);
  const values = { trace: 'before' };
  const metadata = zlinkMessageMetadata(values);
  values.trace = 'after';
  assert.equal(metadata.find('trace'), 'before');
  assert.equal(metadata.values.set, undefined);
});

test('prepared singleton constructor and property dependencies stay synchronous', () => {
  class Dependency {}
  class Handler {
    constructor(dependency) {
      this.dependency = dependency;
    }
  }
  Reflect.defineMetadata(PARAMTYPES_METADATA, [Dependency], Handler);
  Reflect.defineMetadata(PROPERTY_DEPS_METADATA, [{ key: 'property', type: Dependency }], Handler);
  const dependency = new Dependency();
  let gets = 0;
  const ref = {
    introspect: () => ({ scope: Scope.DEFAULT }),
    get() {
      gets++;
      return dependency;
    },
    resolve() {
      return Promise.resolve(dependency);
    }
  };
  const factory = prepareNestHandlerFactory(Handler, ref);
  const result = factory(ref, {}, new Map());
  assert.equal(result instanceof Promise, false);
  assert.equal(result.dependency, dependency);
  assert.equal(result.property, dependency);
  assert.equal(gets, 1, 'One scoped lookup per dependency token.');
});

test('prepared request-scoped dependencies retain asynchronous lookup and per-context identity', async () => {
  class Dependency {}
  class Handler {
    constructor(dependency) {
      this.dependency = dependency;
    }
  }
  Reflect.defineMetadata(PARAMTYPES_METADATA, [Dependency], Handler);
  Reflect.defineMetadata(PROPERTY_DEPS_METADATA, [{ key: 'property', type: Dependency }], Handler);
  const ref = {
    introspect: () => ({ scope: Scope.REQUEST }),
    get() {
      throw new Error('Request dependency must resolve in its context.');
    },
    resolve(_type, context) {
      return Promise.resolve(context.dependency);
    }
  };
  const factory = prepareNestHandlerFactory(Handler, ref);
  const first = new Dependency(),
    second = new Dependency();
  const a = await factory(ref, { dependency: first }, new Map());
  const b = await factory(ref, { dependency: second }, new Map());
  assert.equal(a.dependency, first);
  assert.equal(a.property, first);
  assert.equal(b.dependency, second);
  assert.notEqual(a, b);
});

test('completed Nest instances keep one resolve Promise and skip pending activation waits', async () => {
  const register = bridge.registerIntegrationHandlerInstanceScopeFactory;
  const host = bridge.createIntegrationRuntimeHost;
  let factory;
  bridge.registerIntegrationHandlerInstanceScopeFactory = (resolver, prepared) => {
    factory = prepared;
    return register(resolver, prepared);
  };
  bridge.createIntegrationRuntimeHost = () => ({ start() {}, shutdown() {} });
  try {
    createRuntimeHost(
      bridge.createFrameworkRegistration(),
      {
        get: () => undefined,
        introspect: () => ({ scope: Scope.DEFAULT }),
        registerRequestByContextId() {}
      },
      undefined
    );
  } finally {
    bridge.registerIntegrationHandlerInstanceScopeFactory = register;
    bridge.createIntegrationRuntimeHost = host;
  }
  let disposed = 0;
  class Handler {
    dispose() {
      disposed++;
    }
  }
  factory.prepare(Handler);
  const scope = factory.create();
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  hook.enable();
  let pending;
  try {
    pending = scope.resolve(Handler);
  } finally {
    hook.disable();
  }
  assert.equal(promises, 1, 'One Promise represents the resolved instance; no adaptation chain.');
  const instance = await pending;
  assert.equal(await scope.resolve(Handler), instance);
  const settled = Promise.allSettled;
  let waits = 0;
  Promise.allSettled = function (...args) {
    waits++;
    return settled.apply(this, args);
  };
  try {
    await scope.dispose();
  } finally {
    Promise.allSettled = settled;
  }
  assert.equal(waits, 0);
  assert.equal(disposed, 1);
  const next = factory.create();
  assert.notEqual(await next.resolve(Handler), instance);
  await next.dispose();
  assert.equal(disposed, 2);
});
