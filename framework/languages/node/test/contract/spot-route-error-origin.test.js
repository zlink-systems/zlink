'use strict';

// D5: the spot route dispatch error reply carries the framework-origin marker
// (`zlink.origin=framework`) only when the framework itself generated the
// error (missing handler, framework payload decode failure). Application
// handler failures must never carry the marker, so a requester can
// distinguish framework-origin route errors from application errors.

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkSpotRoutePacketDispatch
} = require('../../packages/framework/dist/runtime/spots/spot-route-packet-dispatch');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

const ORIGIN_KEY = 'zlink.origin';

function fakePart(buffer) {
  const data = Buffer.isBuffer(buffer) ? buffer : Buffer.from(buffer);
  return {
    data: () => data,
    size: () => data.length,
    close() {}
  };
}

function encodeRequestParts(packetName, payload) {
  return channelEnvelope.encodeChannelEnvelopeParts(
    1, // ZLinkChannelMessageKind.Request
    'mesh',
    packetName,
    payload,
    undefined,
    undefined,
    undefined,
    'corr-origin-1',
    true
  ).map((part) => fakePart(part));
}

function createReceived(parts) {
  const replies = [];
  return {
    received: {
      parts,
      routingId: 'source-node',
      requestSeq: 7n,
      reply() {
        const operation = {
          message(part) {
            replies.push(part);
            return operation;
          },
          submit() {}
        };
        return operation;
      }
    },
    replies
  };
}

function decodeReplyHeader(replies) {
  assert.ok(replies.length >= 1, 'expected an error reply to be written');
  const first = replies[0];
  const bytes = Buffer.isBuffer(first) ? first : Buffer.from(first.data());
  return JSON.parse(bytes.toString());
}

function createDispatch(packetHandlers) {
  return new ZLinkSpotRoutePacketDispatch({
    packetHandlers,
    nativeSpotId: 'spot-1',
    serial: {
      async execute(work) {
        return work();
      }
    },
    getTarget: () => ({})
  });
}

test('D5 missing spot route handler reply carries the framework origin marker', async () => {
  const dispatch = createDispatch(new Map());
  const { received, replies } = createReceived(encodeRequestParts('RoutePing', { v: 1 }));

  assert.equal(await dispatch.dispatch(received), true);
  const header = decodeReplyHeader(replies);
  assert.equal(header.kind, 5); // Error reply
  assert.equal(header.metadata[ORIGIN_KEY], 'framework');
  assert.equal(header.correlationId, 'corr-origin-1');
});

test('D5 framework payload decode failure reply carries the framework origin marker', async () => {
  class DecodedHandler {
    handle() {
      throw new Error('handler must not run for an undecodable payload');
    }
  }
  const dispatch = createDispatch(new Map([
    ['RoutePing', [{ handlerType: DecodedHandler }]]
  ]));
  const parts = encodeRequestParts('RoutePing', { v: 1 });
  // Corrupt only the body so the header decodes but the framework's payload
  // decoding fails (PayloadDecodeFailed is framework-generated).
  parts[1] = fakePart(Buffer.from('{invalid-json'));
  const { received, replies } = createReceived(parts);

  assert.equal(await dispatch.dispatch(received), true);
  const header = decodeReplyHeader(replies);
  assert.equal(header.kind, 5);
  assert.equal(header.metadata[ORIGIN_KEY], 'framework');
});

test('D5 application handler failure reply never carries the framework origin marker', async () => {
  class FailingHandler {
    handle() {
      throw new Error('application failure');
    }
  }
  const dispatch = createDispatch(new Map([
    ['RoutePing', [{ handlerType: FailingHandler }]]
  ]));
  const { received, replies } = createReceived(encodeRequestParts('RoutePing', { v: 1 }));

  assert.equal(await dispatch.dispatch(received), true);
  const header = decodeReplyHeader(replies);
  assert.equal(header.kind, 5);
  assert.equal(header.errorMessage, 'application failure');
  assert.equal(Object.prototype.hasOwnProperty.call(header.metadata ?? {}, ORIGIN_KEY), false);
});

test('D5 successful spot route reply carries no origin marker', async () => {
  class OkHandler {
    handle() {
      return { ok: true };
    }
  }
  const dispatch = createDispatch(new Map([
    ['RoutePing', [{ handlerType: OkHandler }]]
  ]));
  const { received, replies } = createReceived(encodeRequestParts('RoutePing', { v: 1 }));

  assert.equal(await dispatch.dispatch(received), true);
  const header = decodeReplyHeader(replies);
  assert.equal(header.kind, 2); // Response
  assert.equal(Object.prototype.hasOwnProperty.call(header.metadata ?? {}, ORIGIN_KEY), false);
});

// D5 (client side of the same contract): the route requester's completion
// path decodes the server-written reply above via decodeChannelReply, which
// must surface `.origin` classified as 'framework' (marker present) or
// 'application' (marker absent) -- never leave it undefined/'none' for a
// reply that genuinely round-tripped a remote peer. Residual-convergence fix:
// Node previously left `.origin` unset whenever the marker was absent,
// unlike .NET's ZLinkErrorOriginWire.RemoteReplyOrigin, which always
// classifies a decoded remote reply as framework or application.
test('D5 route requester decode classifies a framework-marked error reply as origin=framework', () => {
  const framed = channelEnvelope.encodeChannelErrorReplyParts(
    { channelName: 'mesh', messageName: 'RoutePing', correlationId: 'corr-origin-2' },
    new (require('../../packages/framework/dist').ZLinkFrameworkException)(
      0 /* NotFound */,
      'no route handler registered'
    ),
    { [ORIGIN_KEY]: 'framework' }
  );
  assert.throws(
    () => channelEnvelope.decodeChannelReply(framed.map((part) => fakePart(part))),
    (error) => error.origin === 'framework'
  );
});

test('D5 route requester decode classifies an unmarked (application) error reply as origin=application, never none/unspecified', () => {
  const framed = channelEnvelope.encodeChannelErrorReplyParts(
    { channelName: 'mesh', messageName: 'RoutePing', correlationId: 'corr-origin-3' },
    new (require('../../packages/framework/dist').ZLinkFrameworkException)(
      4 /* Rejected */,
      'application spot route failure'
    )
  );
  assert.throws(
    () => channelEnvelope.decodeChannelReply(framed.map((part) => fakePart(part))),
    (error) => error.origin === 'application'
  );
});
