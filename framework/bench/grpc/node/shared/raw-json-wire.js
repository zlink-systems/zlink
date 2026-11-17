// SPDX-License-Identifier: Elastic-2.0
'use strict';

const zlink = require('@zlink-systems/zlink');
const { gameJsonContract } = require('./game-payload');
const {
  stringifyFrameworkJsonV1,
  parseFrameworkJsonV1
} = require('../../../../languages/node/packages/framework/dist/runtime/messaging/framework-json-v1');

const REQUEST_ENVELOPE = Buffer.from(
  '{"kind":1,"channelName":"bench","messageName":"BenchGameMessage",' +
    '"contentType":"application/json","correlationId":null,"deadline":null,' +
    '"topic":null,"errorCode":null,"errorMessage":null,"source":null}',
  'utf8'
);
const RESPONSE_ENVELOPE = Buffer.from(
  '{"kind":2,"channelName":"bench","messageName":"BenchGameMessage",' +
    '"contentType":"application/json","correlationId":null,"deadline":null,' +
    '"topic":null,"errorCode":null,"errorMessage":null,"source":null}',
  'utf8'
);

function encodeGameMessage(value) {
  return zlink.Message.from(
    Buffer.from(stringifyFrameworkJsonV1(value, gameJsonContract.payload), 'utf8')
  );
}

function decodeGameMessage(bytes) {
  return parseFrameworkJsonV1(
    Buffer.from(bytes).toString('utf8'),
    undefined,
    gameJsonContract.payload
  );
}

module.exports = { REQUEST_ENVELOPE, RESPONSE_ENVELOPE, encodeGameMessage, decodeGameMessage };
