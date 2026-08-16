const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const {
  submitRequestOperation
} = require('../../packages/framework/dist/runtime/channels/channel-multipart');
const {
  ZLinkBackendResultError,
  RequestResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');

function throwingRequest(result) {
  return {
    async submit() {
      throw new ZLinkBackendResultError('request', result);
    }
  };
}

test('ClientServer request terminals map to the spec public kind (not collapsed to Unavailable)', async () => {
  const cases = [
    [RequestResult.TimedOut, framework.ZLinkFrameworkErrorKind.DeadlineExceeded],
    [RequestResult.NotFound, framework.ZLinkFrameworkErrorKind.NotFound],
    [RequestResult.Terminated, framework.ZLinkFrameworkErrorKind.ShuttingDown],
    [RequestResult.ProtocolError, framework.ZLinkFrameworkErrorKind.ProtocolError],
    [RequestResult.InternalError, framework.ZLinkFrameworkErrorKind.InternalFailure],
    [RequestResult.Rejected, framework.ZLinkFrameworkErrorKind.Rejected],
    [RequestResult.Conflict, framework.ZLinkFrameworkErrorKind.Unavailable],
    [RequestResult.Busy, framework.ZLinkFrameworkErrorKind.Unavailable],
    [RequestResult.Backpressured, framework.ZLinkFrameworkErrorKind.CapacityExceeded],
    [RequestResult.NotConnected, framework.ZLinkFrameworkErrorKind.Unavailable],
    [RequestResult.InvalidArgument, framework.ZLinkFrameworkErrorKind.InvalidOperation],
    [RequestResult.InvalidState, framework.ZLinkFrameworkErrorKind.InvalidOperation],
    [RequestResult.NotSupported, framework.ZLinkFrameworkErrorKind.InternalFailure]
  ];
  for (const [result, expectedKind] of cases) {
    await assert.rejects(
      () => submitRequestOperation(throwingRequest(result), 'ClientServer request'),
      (error) => {
        assert.equal(
          error instanceof framework.ZLinkFrameworkException,
          true,
          `result ${result} should raise a framework exception`
        );
        assert.equal(error.kind, expectedKind, `result ${result} -> ${expectedKind}`);
        return true;
      }
    );
  }
});

test('malformed reply rejections synthesize ProtocolError, not NotConnected (round-12)', () => {
  //  Spec 32-framework-error-model:58-60 + 91-92 — a reply that could not be
  //  decoded while awaiting a node/channel request must surface publicly as
  //  ProtocolError; only genuine transport rejections collapse to
  //  NotConnected (public Unavailable). The generic completion path formerly
  //  discarded the decode error and synthesized NotConnected for every
  //  rejection.
  const {
    genericOperationFailure
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
  const {
    ServiceWireProtocolError
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

  assert.deepEqual(
    genericOperationFailure(new ServiceWireProtocolError('Invalid reply parts.')),
    { terminalResult: RequestResult.ProtocolError, failureCode: 0 }
  );
  //  Genuine transport rejection stays NotConnected (-> public Unavailable via
  //  the terminal table pinned above).
  assert.deepEqual(
    genericOperationFailure(new Error('socket closed')),
    { terminalResult: RequestResult.NotConnected, failureCode: 0 }
  );
});
