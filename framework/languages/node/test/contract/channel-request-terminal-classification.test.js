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
    [RequestResult.Conflict, framework.ZLinkFrameworkErrorKind.CapacityExceeded],
    [RequestResult.Busy, framework.ZLinkFrameworkErrorKind.CapacityExceeded],
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
