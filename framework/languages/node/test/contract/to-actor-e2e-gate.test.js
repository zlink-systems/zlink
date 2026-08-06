const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

test('TA-B1 sends a missing logical ActorId through the framework caller', () => {
  const client = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/ToActorMessaging/Client/Scenarios/ta-b1-missing-actor-scenario.ts'
  ), 'utf8');
  const actorServer = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/ToActorMessaging/Server/Actor/main.ts'
  ), 'utf8');
  assert.match(client, /ta-b1-missing/);
  assert.match(client, /assertFailure\([^;]+true\)/s);
  assert.match(client, /assertFailure\([^;]+false\)/s);
  assert.match(client, /requireNoEvidence/);
  assert.match(actorServer, /path: '\/actors\/ta-b1-reference\/ensure'/);
  assert.match(actorServer, /path: '\/actors\/ta-b1-reference\/destroy'/);
});
