// SPDX-License-Identifier: MPL-2.0
'use strict';

const zlink = require('@zlink-systems/zlink');

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
  return zlink.Message.from(Buffer.from(JSON.stringify(value), 'utf8'));
}

function decodeGameMessage(bytes) {
  return JSON.parse(Buffer.from(bytes).toString('utf8'));
}

module.exports = { REQUEST_ENVELOPE, RESPONSE_ENVELOPE, encodeGameMessage, decodeGameMessage };
