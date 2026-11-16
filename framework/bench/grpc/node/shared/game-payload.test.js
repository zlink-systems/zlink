// SPDX-License-Identifier: MPL-2.0
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const gamePayload = require('./game-payload');
const rawJson = require('./raw-json-wire');
const {
  stringifyFrameworkJsonV1
} = require('../../../../languages/node/packages/framework/dist/runtime/messaging/framework-json-v1');
const { REQUEST_ENVELOPE, RESPONSE_ENVELOPE } = rawJson;
const { encodeFrameworkPayloadMessage, decodeFrameworkPayloadMessage } = require(
  path.join(
    __dirname,
    '../../../../languages/node/packages/framework/dist/runtime/messaging/payload-codec'
  )
);

for (const payloadSize of [1024, 4096]) {
  test(`raw JSON and framework typed JSON match game fixture at ${payloadSize}`, () => {
    assert.equal(JSON.parse(REQUEST_ENVELOPE).messageName, 'BenchGameMessage');
    assert.equal(JSON.parse(RESPONSE_ENVELOPE).messageName, 'BenchGameMessage');
    const golden = gamePayload.fixture.golden[String(payloadSize)];
    const value = gamePayload.createGameMessage(
      payloadSize,
      golden.run,
      golden.phase,
      golden.seq,
      '1700000000000000000'
    );
    const expected = Buffer.from(golden.json, 'utf8');

    const rawEncoded = rawJson.encodeGameMessage(value);
    try {
      assert.deepEqual(
        Buffer.from(rawEncoded.data()),
        expected,
        'Framework schema codec output bytes'
      );
      const rawDecoded = rawJson.decodeGameMessage(expected);
      assert.equal(rawDecoded.run, 123n);
      assert.equal(rawDecoded.phase, 1n);
      assert.equal(rawDecoded.seq, 456n);
      assert.equal(
        stringifyFrameworkJsonV1(rawDecoded, gamePayload.gameJsonContract.payload),
        golden.json
      );
    } finally {
      rawEncoded.close();
    }

    const frameworkEncoded = encodeFrameworkPayloadMessage(value, undefined, 'BenchGameMessage');
    try {
      assert.deepEqual(
        Buffer.from(frameworkEncoded.data()),
        expected,
        'framework default typed JSON bytes'
      );
      const frameworkDecoded = decodeFrameworkPayloadMessage(
        frameworkEncoded,
        undefined,
        gamePayload.GameMessage,
        'application/json',
        'BenchGameMessage'
      );
      assert.equal(frameworkDecoded.run, 123n);
      assert.equal(frameworkDecoded.phase, 1n);
      assert.equal(frameworkDecoded.seq, 456n);
      assert.equal(
        stringifyFrameworkJsonV1(frameworkDecoded, gamePayload.gameJsonContract.payload),
        golden.json
      );
    } finally {
      frameworkEncoded.close();
    }
    assert.equal(expected.length, golden.byteLength);
  });
}

test('JSON game implementations reject unsupported payload sizes by implementation name', () => {
  for (const implementation of ['zlink-node-json', 'zlink-framework-node-json']) {
    assert.throws(
      () => gamePayload.createGameMessage(2048, '1', '0', '0', '1', implementation),
      new RegExp(`${implementation} supports payload sizes 1024 and 4096`)
    );
  }
});

for (const payloadSize of [1024, 4096]) {
  const valid = JSON.parse(gamePayload.fixture.golden[String(payloadSize)].json);
  const cases = [];
  for (const name of Object.keys(valid)) {
    const missing = structuredClone(valid);
    delete missing[name];
    cases.push([`missing ${name}`, missing]);
    cases.push([`null ${name}`, { ...valid, [name]: null }]);
  }
  for (const name of Object.keys(valid.items[0])) {
    for (const mutation of ['missing', 'null']) {
      const bad = structuredClone(valid);
      if (mutation === 'missing') delete bad.items[0][name];
      else bad.items[0][name] = null;
      cases.push([`${mutation} items[0].${name}`, bad]);
    }
  }
  cases.push(['noncanonical int64', { ...valid, gold: '01' }]);
  cases.push(['numeric int64', { ...valid, gold: 1 }]);
  cases.push(['string int32', { ...valid, heading: '1' }]);
  for (const [label, bad] of cases) {
    test(`JSON codec rejection parity ${payloadSize}: ${label}`, () => {
      const bytes = Buffer.from(JSON.stringify(bad));
      let rawError;
      let frameworkError;
      try {
        rawJson.decodeGameMessage(bytes);
      } catch (error) {
        rawError = error;
      }
      const message = { isEmpty: () => false, getString: () => bytes.toString('utf8') };
      try {
        decodeFrameworkPayloadMessage(
          message,
          undefined,
          gamePayload.GameMessage,
          'application/json',
          'BenchGameMessage'
        );
      } catch (error) {
        frameworkError = error;
      }
      assert.ok(rawError instanceof TypeError, 'binding codec must reject the contract violation');
      assert.ok(frameworkError, 'framework typed codec must reject the contract violation');
      assert.match(frameworkError.message, /PayloadDecodeFailed/);
      assert.equal(rawError.constructor, frameworkError.cause.constructor);
    });
  }
}
