const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('TA-A4 verifies public NotFound after actor destroy', () => {
  const client = fs.readFileSync(path.join(
    root,
    'e2e/ToActorMessaging/Client/Scenarios/ta-a4-disconnect-destroy-scenario.ts'
  ), 'utf8');
  const actor = fs.readFileSync(path.join(root, 'e2e/ToActorMessaging/Server/Actor/main.ts'), 'utf8');

  assert.match(actor, /path: '\/actors\/ta-a4\/destroy'/);
  assert.match(client, /TA-A4-destroyed-request/);
  assert.match(client, /'NotFound'/);
  assert.match(client, /requireNoEvidence\([\s\S]*?'TA-A4-destroyed-request'/);
});
