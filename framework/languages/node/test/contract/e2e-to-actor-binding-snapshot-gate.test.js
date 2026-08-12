const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('TA-A1 merged variants and TA-A3 assert bound-session snapshots', () => {
  const a1 = fs.readFileSync(path.join(
    root,
    'e2e/ToActorMessaging/Client/Scenarios/ta-a1-bound-no-bind-scenario.ts'
  ), 'utf8');
  const a3 = fs.readFileSync(path.join(
    root,
    'e2e/ToActorMessaging/Client/Scenarios/ta-a3-no-bind-then-bind-scenario.ts'
  ), 'utf8');
  const session = fs.readFileSync(path.join(root, 'e2e/ToActorMessaging/Server/Session/main.ts'), 'utf8');
  const runner = fs.readFileSync(path.join(root, 'e2e/ToActorMessaging/run_e2e.sh'), 'utf8');

  assert.match(session, /path: '\/bindings\/snapshot'/);
  assert.match(session, /context\.sessionId/);
  assert.match(a1, /assertSameBinding\(before, await bindingSnapshot/);
  assert.match(a1, /assertUnbound\(await bindingSnapshot[^;]+unbound variant before direct calls/s);
  assert.match(a1, /assertUnbound\(await bindingSnapshot[^;]+unbound variant after direct calls/s);
  assert.match(a1, /TA-A1-unbound-send/);
  assert.match(a1, /TA-A1-unbound-request/);
  assert.match(a3, /assertUnbound\(await bindingSnapshot[^;]+before no-bind calls/s);
  assert.match(a3, /assertBound\(await bindingSnapshot[^;]+after bind/s);
  assert.match(runner, /--session-url "\$SESSION_URL"/);
});
