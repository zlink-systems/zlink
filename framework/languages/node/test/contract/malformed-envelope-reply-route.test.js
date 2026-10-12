const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const { ZLinkChannelRequestDispatcher, ZLinkRoutePacketDispatcher } = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const { ReceiveKind } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

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
          flow: { flowCreationEnabled: () => false },
          report: (event) => errors.push(event)
        };
        const dispatcher = surface === 'channel'
          ? new ZLinkChannelRequestDispatcher({ channelName: 'play', dispatchErrors, handlers: new Map() })
          : new ZLinkRoutePacketDispatcher({ routerChannelId: 'play', dispatchErrors, handlers: [] });
        const parts = [header, 'request'].map((value) => ({ data: () => Buffer.from(value), close() {} }));
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
        assert.equal(replies.length, action === 'reply_error' ? 1 : 0);
        if (replies.length) {
          const result = JSON.parse(Buffer.from(replies[0][0]).toString());
          assert.equal(result.kind, 5);
          assert.equal(result.errorCode, 'protocol_error');
          assert.equal(result.correlationId, JSON.parse(header).correlationId);
        }
        assert.equal(errors.length, 1);
        assert.equal(errors[0].reason, 'invalid_frame');
        assert.equal(errors[0].action, action);
      });
    }
  }
}
