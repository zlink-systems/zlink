const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

test('Nest Session handler awaits public Spot creation after stream authentication in test mode', () => {
  const result = spawnSync(
    process.execPath,
    [path.join(__dirname, 'fixtures/auto-connect-login-process.js')],
    {
      env: { ...process.env, NODE_ENV: 'test' },
      encoding: 'utf8'
    }
  );
  assert.equal(result.signal, null, result.stderr);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /LOGIN_LOBBY_COMPLETE/u);
});
