const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

test('TA-B1 sends a well-formed missing ActorRef through the framework caller', () => {
  const client = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/ToActorMessaging/Client/Scenarios/ta-b1-missing-actor-scenario.ts'
  ), 'utf8');
  const actorServer = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/ToActorMessaging/Server/Actor/main.ts'
  ), 'utf8');
  assert.match(client, /ensureActor\(options, 'ta-b1-reference'\)/);
  assert.match(client, /actors\/ta-b1-reference\/destroy/);
  assert.match(client, /assertFailure\([^;]+reference\.actor\s*\)/s);
  assert.doesNotMatch(client, /assertFailure\([^;]+true\)/s);
  assert.match(actorServer, /path: '\/actors\/ta-b1-reference\/ensure'/);
  assert.match(actorServer, /path: '\/actors\/ta-b1-reference\/destroy'/);
});
