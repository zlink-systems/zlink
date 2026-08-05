const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('RC-A6 launches all three invalid registration axes', () => {
  const scenario = fs.readFileSync(path.join(
    root,
    'e2e/RegistrationCodec/Client/Scenarios/InvalidRegistrationScenario.ts'
  ), 'utf8');
  const host = fs.readFileSync(path.join(
    root,
    'e2e/RegistrationCodec/Server/InvalidDuplicate/invalid-duplicate-host-factory.ts'
  ), 'utf8');

  for (const invalidCase of ['duplicate', 'missing-handler-group', 'mixed-channel-kinds']) {
    assert.match(scenario, new RegExp(invalidCase));
    assert.match(host, new RegExp(invalidCase));
  }
  assert.match(scenario, /expectStartupFailure/g);
});
