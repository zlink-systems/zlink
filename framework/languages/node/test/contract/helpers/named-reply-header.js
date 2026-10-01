'use strict';

const wire = require('../../../packages/stream-wire/dist');
const REQUEST_KIND = 2;
const KIND_OFFSET = 1;

function namedReplyHeader(kind) {
  // Encode the name and optional fields through the Request path, then replace
  // only the kind byte to construct a deliberately invalid reply.
  const header = wire.encodeStreamWireHeader({
    kind: REQUEST_KIND,
    codec: wire.ZlinkStreamCodec.Json,
    flags: 0,
    requestSeq: 1n,
    name: 'A',
    metadata: new Map()
  });
  header[KIND_OFFSET] = kind;
  return header;
}

module.exports = { namedReplyHeader };
