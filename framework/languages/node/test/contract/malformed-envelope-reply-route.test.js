const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const { ZLinkChannelRequestDispatcher, ZLinkRoutePacketDispatcher } = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const { ReceiveKind } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { encodeChannelHeader, decodeChannelHeader } = require('./helpers/protobuf-channel-header');

const rows = fs.readFileSync(path.resolve(__dirname, '../../../../test/fixtures/malformed-envelope-reply-route.tsv'), 'utf8')
  .trim().split(/\r?\n/).filter((row) => row && !row.startsWith('#')).map((row) => row.split('\t'));
assert.equal(rows.length, 22);
for (const [id, request, header, action] of rows) {
  for (const surface of ['channel', 'node']) {
    for (const mesh of [false, true]) {
      test(`malformed reply route ${id} ${surface} ${mesh ? 'mesh' : 'router'}`, async () => {
        const errors = [];
        const replies = [];
        const dispatchErrors = {
          flow: { flowCreationEnabled: () => false, begin: () => undefined },
          report: (event) => errors.push(event)
        };
        const dispatcher = surface === 'channel'
          ? new ZLinkChannelRequestDispatcher({ channelName: 'play', dispatchErrors, handlers: new Map() })
          : new ZLinkRoutePacketDispatcher({ routerChannelId: 'play', dispatchErrors, handlers: [] });
        let fixtureHeader;
        try { fixtureHeader = JSON.parse(header); } catch { fixtureHeader = {}; }
        const headerBytes = encodeFixtureHeader(id, fixtureHeader);
        const parts = [headerBytes, Buffer.from('request')].map((value) => ({ data: () => Buffer.from(value), close() {} }));
        const transportRequest = request === 'true';
        if (mesh) {
          await dispatcher.dispatchMesh({
            kind: transportRequest ? (surface === 'channel' ? ReceiveKind.ChannelRequest : ReceiveKind.NodeRequest)
              : (surface === 'channel' ? ReceiveKind.ChannelSend : ReceiveKind.NodeSend),
            parts, sourceNodeRid: 'source',
            reply: (replyParts) => { replies.push(replyParts); return SubmitResult.Ok; }
          });
        } else {
          const replyParts = [];
          const operation = {
            message: (part) => { replyParts.push(part); return operation; },
            submit: () => { replies.push(replyParts); return { admitted: Promise.resolve() }; }
          };
          await dispatcher.dispatch({ parts, routingId: 'source', replyToken: transportRequest ? {} : null },
            { reply: () => operation });
        }
        const expectReply = action === 'reply_error' && id !== 'correlation-control';
        assert.equal(replies.length, expectReply ? 1 : 0);
        if (replies.length) {
          const result = decodeChannelHeader(Buffer.from(replies[0][0]));
          assert.equal(result.kind, 5);
          assert.equal(result.errorCode, id === 'content-type' ? 'not_found' : 'protocol_error');
          assert.equal(result.correlationId, fixtureHeader.correlationId);
        }
        assert.equal(errors.length, 1);
        assert.equal(errors[0].reason, id === 'content-type' ? 'no_handler' : 'invalid_frame');
        const expectedAction = id === 'correlation-control' ? 'drop' : action;
        assert.equal(errors[0].action, expectedAction);
      });
    }
  }
}

function encodeFixtureHeader(id, header) {
  if (id === 'syntax' || id === 'trailing-json') return Buffer.from([0xff]);
  if (['marker', 'marker-fraction', 'send-marker'].includes(id)) header.kind = 99;
  if (['kind-type', 'kind-value', 'kind-boolean', 'kind-fraction', 'kind-overflow'].includes(id)) {
    header.kind = 99;
  }
  if (id === 'content-type') header.contentType = '';
  if (
    id.startsWith('metadata') ||
    id === 'send-metadata' ||
    id === 'send-header-request' ||
    id.startsWith('correlation-64') ||
    id.startsWith('correlation-space') ||
    id.startsWith('correlation-control') ||
    id === 'correlation-null' ||
    id === 'correlation-number'
  ) {
    header.metadata = [
      { key: 'bad', value: 'first' },
      { key: 'bad', value: 'last' }
    ];
  }
  if (id === 'correlation-number') delete header.correlationId;
  return encodeChannelHeader(header);
}
