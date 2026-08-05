import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');
// Common E2E uses both short IDs such as RM-A1 and Config 12--14 IDs such
// as CH-E2E-07A, SA-E2E-14 and IS-E2E-36. Keep the parser aligned with the
// complete common inventory so an unimplemented configuration cannot be
// omitted from the gate by its naming shape.
const scenarioIdPattern = /\b[A-Z]{2,3}-(?:E2E-[0-9]+[A-Z]?|[A-Z][0-9]+[A-Z]?)\b/g;

test('every Node e2e scenario starts with its verification intent', () => {
  const files = scenarioFiles(path.join(root, 'e2e'));
  const canonicalTitles = scenarioTitles(path.resolve(root, '../../doc/framework/common/e2e'));
  const implementedIds = new Set();

  for (const file of files) {
    const source = fs.readFileSync(file, 'utf8');
    const ids = [...new Set(source.match(scenarioIdPattern) ?? [])];
    assert.equal(ids.length, 1, `${file} must contain exactly one scenario id`);

    const firstLine = source.split(/\r?\n/, 1)[0];
    assert.match(
      firstLine,
      /^\/\/ [A-Z]{2,3}-(?:E2E-[0-9]+[A-Z]?|[A-Z][0-9]+[A-Z]?): .+\.$/,
      `${file} header`
    );
    assert.ok(firstLine.startsWith(`// ${ids[0]}: `), `${file} header must name ${ids[0]}`);
    assert.equal(implementedIds.has(ids[0]), false, `${ids[0]} must have exactly one Node scenario file`);
    implementedIds.add(ids[0]);
    const canonicalTitle = canonicalTitles.get(ids[0]);
    if (canonicalTitle !== undefined) {
      assert.equal(firstLine, `// ${ids[0]}: ${canonicalTitle} 시나리오를 검증한다.`);
    }
  }

  const missing = [...canonicalTitles.keys()].filter((id) => !implementedIds.has(id)).sort();
  const obsolete = [...implementedIds].filter((id) => id.startsWith('ATD-')).sort();
  assert.deepEqual(missing, []);
  assert.deepEqual(obsolete, []);
});

function scenarioFiles(e2eRoot) {
  const files = [];
  for (const config of fs.readdirSync(e2eRoot, { withFileTypes: true })) {
    if (!config.isDirectory()) continue;
    const directory = path.join(e2eRoot, config.name, 'Client', 'Scenarios');
    if (!fs.existsSync(directory)) continue;
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      if (entry.isFile() && entry.name.endsWith('.ts')) files.push(path.join(directory, entry.name));
    }
  }
  return files.sort();
}

function scenarioTitles(commonE2eRoot) {
  const titles = new Map();
  for (const entry of fs.readdirSync(commonE2eRoot, { withFileTypes: true })) {
    if (!entry.isFile() || !/^config-.*\.ko\.md$/.test(entry.name)) continue;
    const source = fs.readFileSync(path.join(commonE2eRoot, entry.name), 'utf8');
    for (const match of source.matchAll(
      /^####\s+([A-Z]{2,3}-(?:E2E-[0-9]+[A-Z]?|[A-Z][0-9]+[A-Z]?))\s+(.+)$/gm
    )) {
      if (!titles.has(match[1])) titles.set(match[1], match[2].trim());
    }
  }
  return titles;
}
