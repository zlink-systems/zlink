'use strict';

const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const packageRoot = path.resolve(__dirname, '../../packages');
function source(relativePath) {
  return readFileSync(path.join(packageRoot, relativePath), 'utf8');
}

test('Redis Lua and provider parsers share one result vocabulary', () => {
  const scripts = source('framework-locations-redis/src/opaque-redis-scripts.ts');
  const parser =
    source('framework-locations-redis/src/opaque-store.ts') +
    source('framework-locations-redis/src/relocation-store.ts');
  assert.equal(
    /return \{['"](?:conflict|backlog|expired|capacity|alreadyStored|stored)['"]/.test(scripts),
    false,
    'Lua result token copies'
  );
  assert.equal(
    /(?:outcome|kind)\s*[!=]==?\s*['"](?:conflict|backlog|applied|expired|capacity|page|alreadyStored)['"]/.test(
      parser
    ),
    false,
    'Redis result comparisons'
  );
});

test('HTTP compression and cookie parsing use their owner vocabulary', () => {
  assert.equal(
    /=== ['"](?:gzip|deflate)['"]/.test(source('http-client/src/runtime/response-body-reader.ts')),
    false,
    'compression token comparisons'
  );
  assert.equal(
    /=== ['"](?:path|secure|max-age)['"]/.test(source('http-client/src/runtime/cookie-jar.ts')),
    false,
    'cookie attribute comparisons'
  );
});

test('inventory storage kinds have one definition used by writers and decoders', () => {
  const inventory = source('framework/src/runtime/locations/aggregate-inventory-store.ts');
  for (const kind of ['aggregate-inventory-page-v1', 'aggregate-inventory-root-v1']) {
    assert.equal(inventory.split(kind).length - 1, 1, kind);
  }
});

test('owner checks reject duplicated tokens and raw decision comparisons', () => {
  assert.throws(() =>
    assert.doesNotMatch(
      "return {'conflict', nowMs}",
      /return \{['"](?:conflict|backlog|expired|capacity|alreadyStored|stored)['"]/
    )
  );
  assert.throws(() =>
    assert.doesNotMatch(
      "if (outcome === 'applied') {}",
      /(?:outcome|kind)\s*[!=]==?\s*['"](?:conflict|backlog|applied|expired|capacity|page|alreadyStored)['"]/
    )
  );
  assert.throws(() => assert.doesNotMatch("encoding === 'gzip'", /=== ['"](?:gzip|deflate)['"]/));
  assert.throws(() =>
    assert.doesNotMatch("attribute === 'secure'", /=== ['"](?:path|secure|max-age)['"]/)
  );
});

test('STREAM media types have one definition shared by both directions and payload encoding', () => {
  const combined =
    source('stream-wire/src/index.ts') +
    source('framework/src/runtime/streams/protocol.ts') +
    source('framework/src/runtime/messaging/payload-codec.ts');
  for (const contentType of [
    'application/json',
    'application/octet-stream',
    'application/x-msgpack',
    'application/x-protobuf'
  ]) {
    assert.equal(combined.split(contentType).length - 1, 1, contentType);
  }
});

test('STREAM codec mapping preserves round trips and rejects unsupported boundary inputs', () => {
  const framework = require('../../packages/framework/dist/internal');
  for (const [codec, contentType] of [
    [framework.ZLinkStreamCodec.Raw, 'application/octet-stream'],
    [framework.ZLinkStreamCodec.Json, 'application/json'],
    [framework.ZLinkStreamCodec.MessagePack, 'application/x-msgpack'],
    [framework.ZLinkStreamCodec.Protobuf, 'application/x-protobuf']
  ]) {
    assert.equal(framework.streamCodecContentType(codec), contentType);
    assert.equal(framework.streamCodecForContentType(contentType), codec);
  }
  for (const codec of [-1, 4, '1', null, undefined, NaN, new Number(1)]) {
    assert.throws(() => framework.streamCodecContentType(codec), TypeError);
  }
  for (const contentType of ['toString', '__proto__', 'constructor', '', null, undefined, new String('application/json')]) {
    assert.throws(() => framework.streamCodecForContentType(contentType), TypeError);
  }
});
