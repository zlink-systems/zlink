'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

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
  for (const contentType of [
    'toString',
    '__proto__',
    'constructor',
    '',
    null,
    undefined,
    new String('application/json')
  ]) {
    assert.throws(() => framework.streamCodecForContentType(contentType), TypeError);
  }
});
