const assert = require('node:assert/strict');
const test = require('node:test');
require('reflect-metadata');
const { prepareNestHandlerFactory } = require('../../packages/nestjs/dist/providers');
const { createManualSendHandlers } = require('../../packages/nestjs/dist/handler-adapters');

test('registered manual handler prepares metadata before dispatch and constructs before suspension', async () => {
  let constructed = 0;
  class Handler {
    constructor() { constructed++; }
    async handle(message) { assert.equal(message, 'payload'); }
  }
  const [registration] = createManualSendHandlers(
    [{ handlerType: Handler, packetName: 'message' }],
    { registerRequestByContextId() {} }
  );
  const original = Reflect.getMetadata;
  let metadataReads = 0;
  Reflect.getMetadata = (...args) => { metadataReads++; return original(...args); };
  try {
    const completion = registration.handler.handle(Buffer.from('"payload"'), {});
    const constructedBeforeSuspension = constructed;
    await completion;
    assert.equal(constructedBeforeSuspension, 1);
    assert.equal(metadataReads, 0);
  } finally {
    Reflect.getMetadata = original;
  }
});

test('prepared Nest factory constructs within the caller and retains no scoped instance', () => {
  let constructed = 0;
  class Handler {
    constructor() {
      constructed++;
    }
  }
  const factory = prepareNestHandlerFactory(Handler);
  const original = Reflect.getMetadata;
  Reflect.getMetadata = () => {
    throw new Error('Dispatch must not inspect handler metadata');
  };
  try {
    const first = factory({}, {}, new Map());
    assert.equal(constructed, 1);
    const second = factory({}, {}, new Map());
    assert.equal(constructed, 2);
    assert.ok(first instanceof Handler);
    assert.notEqual(first, second);
  } finally {
    Reflect.getMetadata = original;
  }
});

test('prepared constructor and property dependencies share the current scope only', async () => {
  const { PROPERTY_DEPS_METADATA } = require('@nestjs/common/constants');
  class Dependency {}
  class Handler {
    constructor(dependency) { this.dependency = dependency; }
  }
  Reflect.defineMetadata('design:paramtypes', [Dependency], Handler);
  Reflect.defineMetadata(PROPERTY_DEPS_METADATA, [{ key: 'property', type: Dependency }], Handler);
  const factory = prepareNestHandlerFactory(Handler);
  let resolutions = 0;
  const resolver = {
    async resolve(token) {
      assert.equal(token, Dependency);
      resolutions++;
      return new Dependency();
    }
  };
  const scope = new Map();
  const first = await factory(resolver, {}, scope);
  const second = await factory(resolver, {}, scope);
  const other = await factory(resolver, {}, new Map());
  assert.equal(resolutions, 2);
  assert.notEqual(first, second);
  assert.equal(first.dependency, first.property);
  assert.equal(first.dependency, second.dependency);
  assert.notEqual(first.dependency, other.dependency);
});
