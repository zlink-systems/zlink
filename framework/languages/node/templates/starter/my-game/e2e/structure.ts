import { readFileSync, realpathSync } from 'node:fs';
import { isAbsolute, relative, resolve, sep, win32 } from 'node:path';
import assert from 'node:assert/strict';
import { Scenarios, NodeEntrypoints } from '../server/shared/constants';
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
    if (node.role === 'session') realpathSync(resolve(root, NodeEntrypoints[node.role]));
    text(node.title);
    if (node.role === 'external') {
      assert.equal(node.config, null);
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
      assert.equal(node.role, 'session', 'No entrypoint for role');
      text(cfg.streamEndpoint);
    }
  }
  assert(
    nodes.some((n) => n.role === 'session'),
    'At least one application node must be declared'
  );
  assert.equal(array(s.channels).length, 0, 'No application channels registered');
  assert.equal(array(s.spots).length, 0, 'No application Spots registered');
  const edges = array(s.edges).map(object);
  for (const edge of edges) {
    owner(edge.from);
    owner(edge.to);
    text(edge.label);
    if (edge.semantic !== undefined) text(edge.semantic);
  }
  console.log('Structure: valid');
}
