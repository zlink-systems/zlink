const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const source = fs.readFileSync(path.join(__dirname, '../Runner/sample-runner.mjs'), 'utf8');
const start = source.indexOf('function assertZoneLayout(');
const end = source.indexOf('\nfunction assertGeneratedRoutingIds', start);
const validate = new Function('logicalZoneIds', 'zoneNodeCapacities', `${source.slice(start, end)}; return assertZoneLayout;`)(
  ['zone-nw', 'zone-ne', 'zone-sw', 'zone-se'],
  new Map([['zone-node-1', 1], ['zone-node-2', 3]])
);

test('capacity 1/3 accepts every singleton placement and either report order', () => {
  const zones = ['zone-nw', 'zone-ne', 'zone-sw', 'zone-se'];
  for (const singleton of zones) {
    const nodes = [
      { nodeId: 'zone-node-1', zones: [singleton] },
      { nodeId: 'zone-node-2', zones: zones.filter((zone) => zone !== singleton) }
    ];
    for (const reports of [nodes, [...nodes].reverse()]) {
      assert.doesNotThrow(() => validate({ nodes: reports, pair: {
        error: null, sourceZoneId: singleton, targetZoneId: zones.find((zone) => zone !== singleton)
      } }));
    }
  }
});

test('capacity layout rejects duplicate, missing, unknown zones and 2/2 placement', () => {
  for (const zones of [
    [['zone-nw'], ['zone-ne', 'zone-sw', 'zone-nw']],
    [['zone-nw'], ['zone-ne', 'zone-sw']],
    [['zone-nw'], ['zone-ne', 'zone-sw', 'unknown']],
    [['zone-nw', 'zone-ne'], ['zone-sw', 'zone-se']]
  ]) {
    assert.throws(() => validate({ nodes: zones.map((owned, index) => ({
      nodeId: `zone-node-${index + 1}`, zones: owned
    })), pair: { error: null, sourceZoneId: 'zone-nw', targetZoneId: 'zone-ne' } }));
  }
});

test('ZW-E4 selects a same-owner boundary for every singleton placement', () => {
  const { adjacentZones } = require('../dist/Server/ZoneNode/Domain/world');
  const { ZoneIds, ZoneWorldSpec, zoneOf } = require('../dist/Shared/spec');
  const client = fs.readFileSync(path.join(__dirname, '../Client/special.ts'), 'utf8');
  const begin = client.indexOf('function sameOwnerPair(');
  const end = client.indexOf('\nasync function diagnose', begin);
  const code = ts.transpileModule(client.slice(begin, end), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS }
  }).outputText;
  const { select } = new Function('adjacentZones', 'ZoneIds', 'ZoneWorldSpec',
    `${code}; return { select: sameOwnerPair };`
  )(adjacentZones, ZoneIds, ZoneWorldSpec);
  const routeCode = ts.transpileModule(fs.readFileSync(path.join(__dirname, '../Client/boundary-route.ts'), 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS }
  }).outputText;
  const routeModule = { exports: {} };
  new Function('require', 'exports', routeCode)((name) => name.includes('Shared/spec')
    ? { ZoneIds, ZoneWorldSpec, zoneOf }
    : require('../dist/Server/ZoneNode/Domain/world'), routeModule.exports);
  const boundary = routeModule.exports.boundaryRoute;
  for (const singleton of Object.values(ZoneIds)) {
    const roster = { nodes: [
      { registered: true, zones: [singleton] },
      { registered: true, zones: Object.values(ZoneIds).filter((zone) => zone !== singleton) }
    ] };
    const selected = select(roster);
    const target = adjacentZones(selected.sourceZoneId).find((zone) => selected.node.zones.includes(zone));
    const route = boundary(target, selected.sourceZoneId);
    assert.equal(zoneOf(route.sourceEdge.x, route.sourceEdge.y), selected.sourceZoneId);
    assert.equal(zoneOf(route.targetInside.x, route.targetInside.y), target);
    assert.ok(selected.node.zones.includes(target));
    assert.ok(Math.abs(route.sourceEdge.x - route.targetInside.x) <= ZoneWorldSpec.maxStepPerAxis);
    assert.ok(Math.abs(route.sourceEdge.y - route.targetInside.y) <= ZoneWorldSpec.maxStepPerAxis);
  }
  assert.throws(() => select({ nodes: Object.values(ZoneIds).map((zone) => ({ registered: true, zones: [zone] })) }));
});

test('Ops finds a cross-owner boundary for every singleton placement', () => {
  const { NodeRegistry } = require('../dist/Server/Ops/node-registry');
  const { adjacentZones } = require('../dist/Server/ZoneNode/Domain/world');
  const { ZoneIds } = require('../dist/Shared/spec');
  const zones = Object.values(ZoneIds);
  for (const singleton of zones) {
    const registry = new NodeRegistry();
    registry.report({ nodeId: 'zone-node-1', zones: [singleton], playerCount: 0, maintenance: false }, 'rid-one');
    registry.report({ nodeId: 'zone-node-2', zones: zones.filter((zone) => zone !== singleton), playerCount: 0, maintenance: false }, 'rid-three');
    const pair = registry.relocationPair();
    assert.ok(pair);
    assert.ok(adjacentZones(pair.sourceZoneId).includes(pair.targetZoneId));
    assert.notEqual(pair.sourceOwnerNodeRid, pair.targetOwnerNodeRid);
    assert.equal(pair.sourceZoneId === singleton, pair.sourceOwnerNodeRid === 'rid-one');
    assert.equal(pair.targetZoneId === singleton, pair.targetOwnerNodeRid === 'rid-one');
  }
});
