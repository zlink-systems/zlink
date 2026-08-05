const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('location-store E2E runners do not accept a shared Redis endpoint', () => {
  const spotServiceRunner = fs.readFileSync(path.join(root, 'e2e/SpotService/run_e2e.sh'), 'utf8');
  const toActorFeatureMap = fs.readFileSync(path.join(root, 'e2e/ToActorMessaging/feature-map.ko.md'), 'utf8');

  assert.doesNotMatch(spotServiceRunner, /ZLINK_REDIS_E2E_ENDPOINT/);
  assert.match(spotServiceRunner, /start_redis_container/);
  assert.doesNotMatch(toActorFeatureMap, /ZLINK_REDIS_E2E_ENDPOINT|외부 Redis/);
});
