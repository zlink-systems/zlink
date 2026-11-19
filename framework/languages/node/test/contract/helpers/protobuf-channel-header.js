'use strict';

const { zlink } = require('../../../packages/framework/dist/runtime/channels/generated/channel-header-v1');
const Header = zlink.framework.channel.v1.ChannelHeader;

const flowOrigin = new Map([
  ['Inbound', 1], ['Timer', 2], ['Application', 3], ['Lifecycle', 4]
]);

function encodeChannelHeader(value) {
  const metadata = Array.isArray(value.metadata)
    ? value.metadata.map(entry => Array.isArray(entry) ? { key: entry[0], value: entry[1] } : entry)
    : Object.entries(value.metadata ?? {}).map(([key, entryValue]) => ({ key, value: entryValue }));
  const header = {
    kind: value.kind ?? 0,
    channelName: value.channelName ?? '',
    messageName: value.messageName ?? '',
    contentType: value.contentType ?? '',
    metadata
  };
  for (const name of ['correlationId', 'deadline', 'errorCode', 'errorMessage', 'flowId', 'source', 'topic']) {
    if (value[name] !== undefined && value[name] !== null) header[name] = value[name];
  }
  const origin = value.flowOrigin;
  if (origin !== undefined && origin !== null) header.flowOrigin = typeof origin === 'string' ? flowOrigin.get(origin) : origin;
  return Buffer.from(Header.encode(header).finish());
}

function decodeChannelHeader(bytes) {
  const decoded = Header.decode(Buffer.from(bytes));
  return {
    kind: decoded.kind,
    channelName: decoded.channelName,
    messageName: decoded.messageName,
    contentType: decoded.contentType,
    correlationId: decoded.correlationId ?? null,
    deadline: decoded.deadline ?? null,
    topic: decoded.topic ?? null,
    errorCode: decoded.errorCode ?? null,
    errorMessage: decoded.errorMessage ?? null,
    source: decoded.source ?? undefined,
    flowId: decoded.flowId ?? undefined,
    flowOrigin: decoded.flowOrigin ?? undefined,
    metadata: Object.fromEntries((decoded.metadata ?? []).map(entry => [entry.key, entry.value]))
  };
}

module.exports = { encodeChannelHeader, decodeChannelHeader };
