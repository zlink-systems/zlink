const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const protobuf = require('../../packages/framework-codec-protobuf/dist/server/framework.cjs');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');
const detachedTaskRunner = new ZLinkRuntimeTaskRunner(
  new ZLinkRuntimeTaskErrorSink(),
  new AbortController().signal
);

test('Public User Spot creation exposes its generation as bigint', async () => {
  let contextGeneration;
  class GenerationSpot {
    async onInitialize() {
      contextGeneration = this.context.objectGeneration;
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(GenerationSpot);
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder
    .addRouteMesh('generation-boundary-public')
    .listen('tcp://127.0.0.1:0')
    .routingId('generation-boundary-public-node')
    .objects()
    .server()
    .addSpotFactory('generation-boundary', GenerationSpot, (factory) =>
      factory.disableRelocation()
    );
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [GenerationSpot] })(
    AppModule
  );
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
    const created = await spots
      .create('generation-boundary')
      .inMesh('generation-boundary-public')
      .submit();
    assert.equal(contextGeneration, created.spot.objectGeneration);
    assert.equal(typeof contextGeneration, 'bigint');
  } finally {
    await app.close();
  }
});

for (const generation of [9007199254740992n, 9223372036854775807n]) {
  test(`Instance context preserves object generation ${generation}`, async () => {
    let contextGeneration;
    class GenerationSpot {
      async onInitialize() {
        contextGeneration = this.context.objectGeneration;
      }
    }
    const manager = new framework.DefaultZLinkSpotManager({
      spotFactories: [],
      detachedTaskRunner,
      instanceSpotFactories: new Map([
        ['generation-boundary.mesh', new Map([['generation-boundary', GenerationSpot]])]
      ])
    });
    const spotId = zlink.RoutingId.from(`generation-boundary-${generation}`);
    try {
      await manager.materializeInstance(
        'generation-boundary.mesh',
        'generation-boundary',
        spotId,
        generation
      );
      assert.equal(contextGeneration, generation);
    } finally {
      await manager.close('generation-boundary.mesh', spotId);
    }
  });
}

for (const [name, bytes] of [
  ['string', [0x08, 0x03, 0x22, 0x05, 0x61]],
  ['bytes', [0x08, 0x06, 0x3a, 0x05, 0x61]],
  ['array entry', [0x08, 0x05, 0x32, 0x08, 0x08, 0x03, 0x22, 0x01, 0x61]],
  ['unknown length-delimited field', [0x08, 0x00, 0x42, 0x05, 0x61]],
  ['unknown fixed64 field', [0x08, 0x00, 0x41, 0x61]],
  ['fixed64 value', [0x08, 0x02, 0x19, 0x61]],
  [
    'length above the safe integer range',
    [0x08, 0x03, 0x22, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x10]
  ],
  ['varint length', [0x08, 0x03, 0x22, 0x80]]
]) {
  test(`Protobuf serializer rejects truncated ${name}`, () => {
    const serializer = protobuf.createProtobufMessageSerializer();
    const payload = framework.ZLinkEncodedPayload.from(Buffer.from(bytes));
    assert.throws(() => serializer.deserialize(payload));
  });
}

test('Protobuf serializer accepts a complete length-delimited value and fixed64 field', () => {
  const serializer = protobuf.createProtobufMessageSerializer();
  for (const original of ['a', Buffer.from('a'), [1, 'a'], {}, { value: 'a' }]) {
    assert.deepEqual(serializer.deserialize(serializer.serialize(original)), original);
  }
});

test('Protobuf serializer preserves every own string key as object data', () => {
  const serializer = protobuf.createProtobufMessageSerializer();
  const original = Object.fromEntries([
    ['__proto__', { marker: 'owned' }],
    ['constructor', 'data'],
    ['prototype', ['nested']],
    ['toString', null],
    ['', true],
    ['한글', 42]
  ]);
  const decoded = serializer.deserialize(serializer.serialize(original));
  assert.deepEqual(decoded, original);
  assert.equal(Object.getPrototypeOf(decoded), Object.prototype);
  for (const key of Object.keys(original)) {
    assert.equal(Object.hasOwn(decoded, key), true);
    assert.deepEqual(Object.getOwnPropertyDescriptor(decoded, key), {
      value: original[key],
      writable: true,
      enumerable: true,
      configurable: true
    });
  }
  assert.deepEqual(serializer.deserialize(serializer.serialize(decoded)), original);
});

test('Protobuf serializer preserves non-enumerable own string keys as object data', () => {
  const serializer = protobuf.createProtobufMessageSerializer();
  const original = Object.create({ inherited: 'excluded' });
  for (const key of ['__proto__', 'hidden']) {
    Object.defineProperty(original, key, { value: key, configurable: true });
  }
  original[Symbol('excluded')] = 'symbol';
  const decoded = serializer.deserialize(serializer.serialize(original));
  assert.deepEqual(Object.getOwnPropertyNames(decoded).sort(), ['__proto__', 'hidden']);
  for (const key of Object.getOwnPropertyNames(original)) {
    assert.equal(Object.hasOwn(decoded, key), true);
    assert.equal(decoded[key], original[key]);
  }
  assert.equal(Object.getPrototypeOf(decoded), Object.prototype);
  assert.equal(Object.getOwnPropertySymbols(decoded).length, 0);
});
