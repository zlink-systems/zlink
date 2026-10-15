import { readFileSync, realpathSync } from 'node:fs';
import { isAbsolute, relative, resolve, sep, win32 } from 'node:path';
import assert from 'node:assert/strict';
import { Names, Scenarios, NodeEntrypoints } from '../server/shared/constants';
function object(value: unknown): Record<string, unknown> {
  assert(value !== null && typeof value === 'object' && !Array.isArray(value), 'Expected object');
  return value as Record<string, unknown>;
}
function text(value: unknown): string {
  assert(typeof value === 'string' && value.trim().length > 0, 'Expected nonempty string');
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
  const ids = nodes.map((n) => text(n.id));
  unique(ids, 'node ID');
  function owner(value: unknown): Record<string, unknown> {
    const node = nodes.find((n) => n.id === text(value));
    assert(node, 'Unknown owner');
    return node;
  }
  for (const node of nodes) {
    text(node.role);
    if (node.role === 'session' || node.role === 'zone' || node.role === 'instance')
      realpathSync(resolve(root, NodeEntrypoints[node.role]));
    text(node.title);
    if (node.config === null) {
      assert.equal(node.role, 'external');
      continue;
    }
    const config = text(node.config);
    assert(
      !isAbsolute(config) && !win32.isAbsolute(config) && !config.includes('\\'),
      'Config path must be relative with /'
    );
    const file = realpathSync(resolve(root, config));
    const rel = relative(root, file);
    assert(
      rel && rel !== '..' && !rel.startsWith('..' + sep) && !isAbsolute(rel),
      'Config escapes project root'
    );
    if (node.role !== 'location-store') {
      const cfg = object(JSON.parse(readFileSync(file, 'utf8')));
      assert.equal(cfg.id, node.id, 'Config ID mismatch');
    }
  }
  const channels = array(s.channels).map(object);
  unique(
    channels.map((c) => `${text(c.name)}:${text(c.owner)}`),
    'channel'
  );
  for (const c of channels) {
    owner(c.owner);
    assert.equal(c.mesh, Names.mesh);
    assert.equal(c.name, Names.border);
    assert.equal(owner(c.owner).role, 'zone');
  }
  const spots = array(s.spots).map(object);
  unique(
    spots.map((n) => text(n.id)),
    'Spot ID'
  );
  const logical: string[] = [];
  for (const spot of spots) {
    const host = owner(spot.owner);
    assert(['zone', 'instance'].includes(text(host.role)));
    assert(['entry', 'user', 'instance'].includes(text(spot.kind)), 'Unknown Spot kind');
    if (spot.kind === 'entry') {
      assert.equal(host.role, 'zone', 'Entry Spot owner must host Zone runtime');
      assert.equal(spot.type, null);
      assert(!('dynamic' in spot) && !('logicalId' in spot));
    } else {
      assert.equal(spot.kind, 'user');
      assert.equal(spot.type, host.role === 'zone' ? Names.zone : Names.dungeon);
      if (spot.dynamic === true) assert(!('logicalId' in spot));
      else {
        assert(!('dynamic' in spot) || spot.dynamic === false);
        logical.push(text(spot.logicalId));
      }
    }
  }
  unique(logical, 'logical Spot ID');
  assert(
    spots.some((s) => s.kind === 'entry'),
    'Missing Entry Spot'
  );
  assert(
    spots.some((s) => s.type === Names.zone),
    'Missing Zone type'
  );
  for (const node of nodes.filter((n) => n.role === 'zone')) {
    const cfg = object(JSON.parse(readFileSync(text(node.config), 'utf8')));
    const configured = array(cfg.zoneIds).map(text);
    unique(configured, 'configured Zone ID');
    const declared = spots
      .filter((s) => s.owner === node.id && s.type === Names.zone)
      .map((s) => text(s.logicalId));
    assert.deepEqual([...declared].sort(), [...configured].sort(), 'ZoneIds mismatch');
    assert(
      Number.isInteger(cfg.zoneCapacity) && Number(cfg.zoneCapacity) >= configured.length,
      'Invalid Zone capacity'
    );
    assert(
      channels.some((c) => c.owner === node.id && c.name === Names.border),
      'Missing border channel'
    );
  }
  assert(
    spots.some((s) => s.type === Names.dungeon),
    'Missing Dungeon type'
  );
  for (const edge of array(s.edges).map(object)) {
    owner(edge.from);
    owner(edge.to);
    text(edge.label);
    if (edge.semantic !== undefined) text(edge.semantic);
  }
  console.log('Structure: valid');
}
