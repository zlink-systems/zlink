import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');

test('Config 9 and 10 keep one client scenario file per documented id', () => {
  assertScenarioLayout('ToActorMessaging', documentedIds('TA', 'config-9-'));
  assertScenarioLayout('SpotActorTransfer', documentedIds('ST', 'config-10-'));
});

function assertScenarioLayout(configName, expectedIds) {
  const client = path.join(root, 'e2e', configName, 'Client');
  const scenarioDirectory = path.join(client, 'Scenarios');
  const supportDirectory = path.join(client, 'Support');
  assert.equal(fs.existsSync(scenarioDirectory), true, `${configName} Scenarios directory`);
  assert.equal(fs.existsSync(supportDirectory), true, `${configName} Support directory`);

  const files = fs.readdirSync(scenarioDirectory)
    .filter((name) => name.endsWith('-scenario.ts'))
    .sort();
  assert.equal(files.length, expectedIds.length, `${configName} scenario file count`);

  const actualIds = files.map((name) => {
    const source = fs.readFileSync(path.join(scenarioDirectory, name), 'utf8');
    const matches = [...new Set(source.match(/\b(?:TA|ST)-[A-Z][0-9]+[A-Z]?\b/g) ?? [])];
    assert.equal(matches.length, 1, `${configName}/${name} scenario id`);
    assert.match(source.split(/\r?\n/, 1)[0], new RegExp(`^// ${matches[0]}: `));
    if (matches[0] === 'ST-H2') {
      assert.match(source, /export async function prepareStH2TargetRestart/);
      assert.match(source, /export async function verifyStH2TargetRestart/);
    } else {
      assert.match(source, /export async function run/);
    }
    return matches[0];
  }).sort();
  assert.deepEqual(actualIds, expectedIds);

  const main = fs.readFileSync(path.join(client, 'main.ts'), 'utf8');
  assert.doesNotMatch(main, /(?:async\s+)?function\s+run(?:Ta)?[A-F][0-9]+\s*\(/);
}

function documentedIds(prefix, filePrefix) {
  const docsDirectory = path.resolve(root, '../../doc/framework/common/e2e');
  const ids = new Set();
  for (const name of fs.readdirSync(docsDirectory)) {
    if (!name.startsWith(filePrefix) || !name.endsWith('.md')) continue;
    const source = fs.readFileSync(path.join(docsDirectory, name), 'utf8');
    for (const match of source.matchAll(new RegExp(`^####\\s+(${prefix}-[A-Z0-9]+)\\b`, 'gm'))) {
      ids.add(match[1]);
    }
  }
  return [...ids].sort();
}
