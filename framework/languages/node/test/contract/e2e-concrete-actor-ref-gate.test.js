const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('P0 actor-ref E2E uses current ActorRef fields without the removed snapshot helper', () => {
  const toActor = fs.readFileSync(path.join(
    root,
    'e2e/ToActorMessaging/Client/Support/actor-scenario-support.ts'
  ), 'utf8');
  const shared = fs.readFileSync(path.join(
    root,
    'e2e/ToActorMessaging/Shared/messages.ts'
  ), 'utf8');

  assert.match(toActor, /response\.actor\.nodeRid\.trim\(\)\.length > 0/);
  assert.match(toActor, /BigInt\(response\.actor\.objectGeneration\) > 0n/);
  assert.match(toActor, /response\.actor\.meshName\.length > 0/);
  assert.match(shared, /interface ActorRefPayload/);
  assert.match(shared, /readonly objectGeneration: string/);
  assert.match(shared, /readonly meshName: string/);
  assert.doesNotMatch(`${toActor}\n${shared}`, /\bActorRefSnapshot\b/);
});
