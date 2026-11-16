'use strict';

const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { decodeChannelHeader } = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { decodeChannelHeader: decodeGeneratedHeader, encodeChannelHeader } = require('./helpers/protobuf-channel-header');

const golden = JSON.parse(readFileSync(
  path.resolve(__dirname, '../../../../runtime/protocol/golden/channel-header-v1.json'),
  'utf8'
));

function message(bytes) {
  return [{ data: () => bytes }];
}

function camelHeader(value) {
  const names = {
    channel_name: 'channelName',
    message_name: 'messageName',
    content_type: 'contentType',
    correlation_id: 'correlationId',
    error_code: 'errorCode',
    error_message: 'errorMessage',
    flow_id: 'flowId',
    flow_origin: 'flowOrigin',
    source: 'source'
  };
  return Object.fromEntries(Object.entries(value).map(([key, field]) => [names[key] ?? key, field]));
}

function snakeHeader(value) {
  const result = {
    kind: value.kind,
    channel_name: value.channelName,
    message_name: value.messageName,
    content_type: value.contentType
  };
  const names = {
    correlationId: 'correlation_id',
    deadline: 'deadline',
    errorCode: 'error_code',
    errorMessage: 'error_message',
    flowId: 'flow_id',
    flowOrigin: 'flow_origin',
    source: 'source',
    topic: 'topic'
  };
  for (const [key, name] of Object.entries(names)) {
    if (value[key] !== null && value[key] !== undefined) result[name] = value[key];
  }
  if (Object.keys(value.metadata).length !== 0) {
    result.metadata = Object.entries(value.metadata).sort(([left], [right]) => left.localeCompare(right));
  }
  return result;
}

test('generated ChannelHeader codec matches every shared golden vector', async t => {
  for (const vector of golden.valid) {
    await t.test(vector.name, () => {
      const bytes = encodeChannelHeader(camelHeader(vector.header));
      assert.equal(bytes.toString('hex'), vector.hex);
      assert.deepEqual(snakeHeader(decodeGeneratedHeader(Buffer.from(vector.hex, 'hex'))), vector.header);
    });
  }
});

test('ChannelHeader validator rejects every shared invalid vector as a protocol error', async t => {
  for (const vector of golden.reject) {
    await t.test(vector.name, () => {
      assert.throws(
        () => decodeChannelHeader(message(Buffer.from(vector.hex, 'hex'))),
        error => error?.kind === 8
      );
    });
  }
});
