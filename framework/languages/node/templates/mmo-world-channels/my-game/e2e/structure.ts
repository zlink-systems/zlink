import { readFileSync, realpathSync } from 'node:fs';
import { isAbsolute, relative, resolve, sep, win32 } from 'node:path';
import assert from 'node:assert/strict';
import { Names, Scenarios, NodeEntrypoints, meshName } from '../server/shared/constants';
import { readConfig } from '../server/shared/config';
function object(value: unknown): Record<string, unknown> {
  assert(value !== null && typeof value === 'object' && !Array.isArray(value), 'Expected object');
  return value as Record<string, unknown>;
}
function text(value: unknown): string {
  assert(typeof value === 'string' && value.trim(), 'Expected nonempty string');
  return value;
}
function array(value: unknown): unknown[] {
  assert(Array.isArray(value), 'Expected array');
  return value;
}
function unique(values: string[], label: string): void {
  assert.equal(new Set(values).size, values.length, `Duplicate ${label}`);
}
export function checkStructure(path = 'zlink.structure.json'): void {
  const root = realpathSync('.');
  const s = object(JSON.parse(readFileSync(path, 'utf8')));
  assert.equal(s.version, 1, 'Unknown structure version');
  for (const key of ['id', 'name', 'language']) text(s[key]);
  assert.equal(s.language, 'node');
  const run = object(s.run),
    commands = object(run.commands);
  assert.deepEqual(Object.keys(commands).sort(), ['powershell', 'sh']);
  for (const command of Object.values(commands)) {
    const a = array(command);
    assert(a.length);
    a.forEach(text);
  }
  assert.deepEqual(run.scenarios, [...Scenarios]);
  const nodes = array(s.nodes).map(object);
  unique(
    nodes.map((n) => text(n.id)),
    'node ID'
  );
  function owner(value: unknown): Record<string, unknown> {
    const node = nodes.find((n) => n.id === text(value));
    assert(node, 'Unknown owner');
    return node;
  }
  const configs = new Map<string, ReturnType<typeof readConfig>>();
  for (const node of nodes) {
    text(node.role);
    text(node.title);
    if (node.config === null) {
      assert.equal(node.role, 'external');
      continue;
    }
    const config = text(node.config);
    assert(
      !isAbsolute(config) && !win32.isAbsolute(config) && !config.includes('\\'),
      'Config must be relative with /'
    );
    const file = realpathSync(resolve(root, config)),
      rel = relative(root, file);
    assert(
      rel && rel !== '..' && !rel.startsWith('..' + sep) && !isAbsolute(rel),
      'Config escapes project root'
    );
    if (node.role === 'world-db') continue;
    assert(
      node.role === 'channel' || node.role === 'session' || node.role === 'accounts',
      'Unknown application role'
    );
    realpathSync(resolve(root, NodeEntrypoints[node.role]));
    const cfg = readConfig(file);
    assert.equal(cfg.id, node.id, 'Config ID mismatch');
    configs.set(cfg.id, cfg);
  }
  const channels = array(s.channels).map(object);
  unique(
    channels.map((c) => `${text(c.name)}:${text(c.owner)}`),
    'channel'
  );
  assert.equal(channels.length, 0, 'No channel registrations');
  const spots = array(s.spots).map(object);
  unique(
    spots.map((n) => text(n.id)),
    'Spot ID'
  );
  const logical: string[] = [];
  for (const spot of spots) {
    const host = owner(spot.owner);
    assert.equal(host.role, 'channel');
    if (spot.kind === 'entry') {
      assert.equal(spot.type, null);
      assert(!('dynamic' in spot) && !('logicalId' in spot));
    } else {
      assert.equal(spot.kind, 'user');
      assert.equal(spot.type, Names.zone);
      assert(spot.dynamic === undefined || spot.dynamic === false);
      const cfg = configs.get(text(host.id))!;
      assert.equal(spot.mesh, meshName(cfg.worldId), 'Zone mesh mismatch');
      logical.push(`${text(spot.mesh)}:${text(spot.logicalId)}`);
    }
  }
  // Logical IDs are scoped to isolated meshes; field@1 is valid in both worlds.
  unique(logical, 'logical Spot identity');
  for (const node of nodes.filter((n) => n.role === 'channel')) {
    const cfg = configs.get(text(node.id))!;
    assert.equal(
      spots.filter((s) => s.owner === node.id && s.kind === 'entry').length,
      1,
      'One Entry per channel host'
    );
    assert.deepEqual(
      spots
        .filter((s) => s.owner === node.id && s.kind === 'user')
        .map((s) => text(s.logicalId))
        .sort(),
      [...cfg.zoneIds].sort(),
      'Zone bootstrap mismatch'
    );
  }
  for (const edge of array(s.edges).map(object)) {
    owner(edge.from);
    owner(edge.to);
    text(edge.label);
    if (edge.semantic !== undefined) text(edge.semantic);
  }
  console.log('Structure: valid');
}
