'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const protocol = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkRuntimeMessageFlowOutcome
} = require('../../packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions');
const {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');

const failures = [
  [
    new framework.ZLinkFrameworkException(framework.ZLinkFrameworkErrorKind.NotFound, 'missing'),
    'no_handler'
  ],
  [
    new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.ProtocolError,
      'invalid'
    ),
    'invalid_frame'
  ],
  [
    new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.ShuttingDown,
      'shutdown'
    ),
    'shutdown'
  ],
  [
    createInternalFrameworkException(ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed, 'decode'),
    'decode_error'
  ],
  [
    createInternalFrameworkException(ZLinkFrameworkInternalErrorKind.SpotGenerationStale, 'stale'),
    'stale_target'
  ],
  [
    createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
      'missing route'
    ),
    'no_handler'
  ],
  [new Error('handler failed'), 'handler_exception']
];

for (const [error, reason] of failures) {
  for (const request of [false, true]) {
    test(`Instance ${request ? 'request' : 'send'} ${error.message} reports one ${reason} terminal`, async () => {
      const diagnostics = [];
      const flow = [];
      const replies = [];
      let targetLookups = 0;
      const manager = new framework.DefaultZLinkSpotManager({
        detachedTaskRunner: new ZLinkRuntimeTaskRunner(
          new ZLinkRuntimeTaskErrorSink(),
          new AbortController().signal
        ),
        spotFactories: [],
        instanceSpotApplicationTargetProvider() {
          // Fail target resolution once; diagnostic enrichment performs the next lookup.
          if (targetLookups++ === 0) throw error;
          return undefined;
        },
        dispatchErrors: {
          captureEnabled: () => true,
          report: (event) => diagnostics.push(event),
          flow: {
            flowCreationEnabled: () => false,
            accepts: () => true,
            trace: (event) => flow.push(event)
          }
        }
      });
      const parts = protocol
        .encodeChannelEnvelopeParts(request ? 1 : 3, 'instance', 'Ping', {})
        .map((part) => zlink.Message.from(part));
      try {
        await manager.dispatchMeshInstance(
          'mesh',
          { spotId: 'room' },
          {
            kind: framework.ReceiveKind.InstanceSpotActivation,
            ...(request ? { operationKind: framework.OperationKind.InstanceSpotRequest } : {}),
            sourceNodeRid: 'source',
            parts,
            reply(replyParts) {
              replies.push(replyParts.map((part) => zlink.Message.from(part)));
              return zlink.SubmitResult.Ok;
            }
          }
        );
        assert.equal(diagnostics.length, 1);
        assert.equal(diagnostics[0].reason, reason);
        assert.equal(diagnostics[0].action, request ? 'reply_error' : 'drop');
        assert.equal(diagnostics[0].error, error);
        assert.equal(replies.length, request ? 1 : 0);
        const dropped = flow.filter(
          (event) => event.outcome === ZLinkRuntimeMessageFlowOutcome.Dropped
        );
        assert.equal(dropped.length, request ? 0 : 1);
        if (!request) assert.equal(dropped[0].errorReason, reason);
      } finally {
        for (const part of [...parts, ...replies.flat()]) part.close();
      }
    });
  }
}
