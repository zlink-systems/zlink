'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const payloadCodec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
const encodedStorage = require(
  '../../packages/framework/dist/contracts/Common/encoded-payload-storage'
);

const fixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/payload-ownership-v1.json'
), 'utf8'));

test('lazy payload ownership consumes the shared copy and decode budget', () => {
  assert.equal(fixture.fixture, 'zlink.framework.payload-ownership');
  assert.equal(fixture.copyBudget.frameworkCopiesAfterOwnership, 0);
  assert.equal(fixture.copyBudget.readonlyAccessorCopies, 0);
  assert.equal(fixture.copyBudget.maximumDeserializationsAfterAdmission, 1);

  const bytes = Buffer.from('fixture-payload');
  let deserializations = 0;
  const decoded = { value: 41 };
  const serializer = {
    serialize: () => assert.fail('receive ownership test must not serialize'),
    deserialize(payload) {
      deserializations += 1;
      assert.equal(encodedStorage.borrowEncodedPayload(payload), bytes);
      return decoded;
    }
  };
  const source = {
    data: () => bytes,
    close: () => undefined,
    copy: () => assert.fail('ownership handoff must not copy'),
    size: () => bytes.length,
    isEmpty: () => false,
    getString: () => bytes.toString('utf8'),
    toBytes: () => assert.fail('ownership handoff must not copy')
  };
  const message = payloadCodec.wrapFrameworkPayloadMessage(
    source,
    new Map([['application/x-zlink-fixture', serializer]]),
    'application/x-zlink-fixture'
  );
  const encoded = message.toEncodedPayload();
  assert.equal(encodedStorage.borrowEncodedPayload(encoded), bytes);

  const reads = [];
  for (let index = 0; index < fixture.accessorScenario.reads; index += 1) {
    reads.push(message.decode(Object));
  }
  assert.equal(deserializations, 1);
  assert.ok(reads.every(value => value === decoded));
  assert.equal(encodedStorage.borrowEncodedPayload(message.toEncodedPayload()), bytes);
});

test('rejected and cancelled fixture paths do not deserialize payloads', () => {
  const noDecodeScenarios = fixture.scenarios.filter(
    scenario => scenario.deserializations === 0
  );
  assert.deepEqual(
    noDecodeScenarios.map(scenario => scenario.name),
    [
      'admission-rejection-before-deserialize',
      'accepted-cancellation-before-handler',
      'accepted-raw-handler-success'
    ]
  );
  for (const scenario of noDecodeScenarios) {
    assert.ok(
      scenario.admitted === false
      || scenario.handlerKind === 'raw'
      || scenario.handlerInvocations === 0,
      scenario.name
    );
  }
});
