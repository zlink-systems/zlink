// SPDX-License-Identifier: MPL-2.0
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const { ZLinkPacket } = require('@zlink-systems/framework');
const gamePayload = require('./game-payload');
const rawJson = require('./raw-json-wire');
const { REQUEST_ENVELOPE, RESPONSE_ENVELOPE } = rawJson;
const { encodeFrameworkPayloadMessage, decodeFrameworkPayloadMessage } = require(
  path.join(
    __dirname,
    '../../../../languages/node/packages/framework/dist/runtime/messaging/payload-codec'
  )
);

ZLinkPacket('BenchGameMessage')(gamePayload.GameMessage);

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
      assert.deepEqual(Buffer.from(rawEncoded.data()), expected, 'JSON.stringify output bytes');
      const rawDecoded = rawJson.decodeGameMessage(expected);
      assert.equal(rawDecoded.run, '123');
      assert.equal(rawDecoded.phase, '1');
      assert.equal(rawDecoded.seq, '456');
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
      assert.equal(frameworkDecoded.run, '123');
      assert.equal(frameworkDecoded.phase, '1');
      assert.equal(frameworkDecoded.seq, '456');
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
