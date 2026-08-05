const assert = require('node:assert/strict');
const test = require('node:test');

const spec = require('../../samples/ZoneWorld/dist/Shared/spec');
const world = require('../../samples/ZoneWorld/dist/Server/ZoneNode/Domain/world');
const { validateMove } = require('../../samples/ZoneWorld/dist/Server/ZoneNode/Domain/move-policy');
const { ZoneState } = require('../../samples/ZoneWorld/dist/Server/ZoneNode/Domain/zone-state');

test('ZoneWorld maps quadrants, nodes, adjacency, and edge-only border bands', () => {
  assert.equal(spec.zoneOf(25, 25), 'zone-nw');
  assert.equal(spec.zoneOf(50, 25), 'zone-ne');
  assert.equal(spec.zoneOf(25, 50), 'zone-sw');
  assert.equal(spec.zoneOf(50, 50), 'zone-se');
  assert.equal(spec.nodeOf('zone-nw'), 'zone-node-1');
  assert.deepEqual(world.adjacentZones('zone-nw'), ['zone-ne', 'zone-sw']);
  assert.equal(world.inBorderBand(45, 45, 'zone-nw', 'zone-ne'), true);
  assert.equal(world.inBorderBand(45, 45, 'zone-nw', 'zone-se'), false);
});

test('ZoneWorld movement reports the first canonical rejection reason', () => {
  assert.deepEqual(validateMove({ x: 25, y: 25 }, -40, 25, () => false), {
    kind: 'rejected', reason: 'OutOfRange'
  });
  assert.deepEqual(validateMove({ x: 25, y: 25 }, 31, 25, () => false), {
    kind: 'rejected', reason: 'TooFar'
  });
  assert.deepEqual(validateMove({ x: 49, y: 49 }, 50, 50, () => false), {
    kind: 'rejected', reason: 'DiagonalCrossing'
  });
  assert.deepEqual(validateMove({ x: 48, y: 25 }, 52, 25, () => true), {
    kind: 'rejected', reason: 'ZoneMaintenance'
  });
});

test('ZoneWorld zone state replaces, expires, merges, and UTF-8 sorts border snapshots', () => {
  const state = new ZoneState('zone-nw');
  state.enter('player-z', 45, 25, false);
  state.applyBorderSnapshot('zone-ne', 2, [
    { playerId: 'player-a', x: 52, y: 25, zoneId: 'zone-ne', isBot: false }
  ]);
  state.applyBorderSnapshot('zone-ne', 1, []);
  assert.deepEqual(state.visiblePlayers().map((player) => player.playerId), ['player-a', 'player-z']);
  assert.deepEqual(state.borderBandFor('zone-ne').map((player) => player.playerId), ['player-z']);
  state.nextTick();
  state.nextTick();
  state.nextTick();
  state.expireStaleSnapshots();
  assert.deepEqual(state.visiblePlayers().map((player) => player.playerId), ['player-z']);
});
