const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

const {
  decodeRoutingId,
  encodeRoutingIdStorageHex,
  routingIdsEqual,
  routingIdWireHex
} = require('../../packages/framework/dist/runtime/routing-id');

test('routing id equality compares canonical bytes without text or hex aliases', () => {
  const binding = zlink.RoutingId.from('play');

  assert.equal(routingIdsEqual('play', binding), true);
  assert.equal(routingIdsEqual('706c6179', 'play'), false);
  assert.equal(routingIdsEqual('PLAY', 'play'), false);
  assert.equal(routingIdsEqual(undefined, undefined), true);
  assert.equal(routingIdsEqual('play', undefined), false);
});

test('routing id storage encoding stays separate from optional wire encoding', () => {
  const binding = zlink.RoutingId.from('play');

  assert.equal(encodeRoutingIdStorageHex('706c6179'), '3730366336313739');
  assert.equal(encodeRoutingIdStorageHex(binding), '706c6179');
  assert.equal(routingIdWireHex('play'), undefined);
  assert.equal(routingIdWireHex(binding), '706c6179');
  assert.equal(String(decodeRoutingId('ignored', '706c6179')), 'play');
});

test('routing id wire decoding preserves opaque bytes that cannot round-trip through display text', () => {
  const decoded = decodeRoutingId('1', '00000001');

  assert.equal(decoded.toHex(), '00000001');
  assert.equal(routingIdsEqual(decoded, zlink.RoutingId.fromHex('00000001')), true);
  assert.equal(routingIdsEqual(decoded, '1'), false);
});
