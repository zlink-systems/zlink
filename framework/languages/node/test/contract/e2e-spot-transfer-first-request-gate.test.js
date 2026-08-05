const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('SpotActorTransfer sends the first request immediately after spot creation', () => {
  const client = fs.readFileSync(path.join(
    root,
    'e2e/SpotActorTransfer/Client/Support/scenario-support.ts'
  ), 'utf8');
  const createSpot = client.match(/export async function createSpot\([^]*?\n}/);

  assert.ok(createSpot, 'SpotActorTransfer createSpot helper is missing.');
  assert.doesNotMatch(createSpot[0], /delay\(|setTimeout|retry/i);
  assert.match(createSpot[0], /return await post<CreateSpotRes>/);
});
