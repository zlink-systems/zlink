const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

test('samples use the exact no-argument Actor dispatch opt-in', () => {
  const source = read('samples/Bingo.Ts/Server/Session/bingo-session-module.ts');
  assert.match(source, /\.enableActorDispatch\(\)/);
  assert.doesNotMatch(source, /\.enableActorDispatch\([^)]/);
});

test('native process fixture pauses one close and proves terminal cleanup', () => {
  const testSource = read('test/contract/user-spot-native-two-process.test.js');
  const fixture = read('test/contract/fixtures/user-spot-native-process.js');

  assert.match(testSource, /await target\.waitForEvent\('close-entered'\)/);
  assert.match(testSource, /await target\.command\('releaseFirstClose'\)/);
  assert.match(testSource, /target\.command\('closeExecutions'\), 1/);
  assert.match(fixture, /ZLINK_TEST_PAUSE_FIRST_CLOSE/);
  assert.match(fixture, /type: 'close-entered'/);
});
