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
    [RequestResult.Backpressured, framework.ZLinkFrameworkErrorKind.DeadlineExceeded],
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

test('request failures retain the Core terminal and classify submit results by meaning', () => {
  //  Core REQUEST result and errno classification belong to Core and the
  //  binding (core 03-errors §Result와 errno 대응). One Framework function
  //  classifies node/channel/stateful request failures: a binding REQUEST
  //  result is kept as is, while submit failures retain their Core meaning.
  const {
    requestFailureResult
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
  const {
    ServiceWireProtocolError
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const {
    OperationCancelledError,
    OperationTimeoutError
  } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
  const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

  for (const result of [
    RequestResult.TimedOut,
    RequestResult.NotFound,
    RequestResult.Terminated,
    RequestResult.Rejected,
    RequestResult.Busy,
    RequestResult.NotConnected,
    RequestResult.Backpressured,
    RequestResult.InternalError
  ]) {
    assert.deepEqual(
      requestFailureResult(new ZLinkBackendResultError('request', result, 113)),
      { terminalResult: result, failureCode: 0 },
      `Core request result ${result} is kept`
    );
  }
  const submitted = (result) =>
    requestFailureResult(new ZLinkBackendResultError('submit', result, 2));
  assert.deepEqual(submitted(SubmitResult.Backpressured), {
    terminalResult: RequestResult.Backpressured,
    failureCode: 0
  });
  assert.deepEqual(submitted(SubmitResult.NotConnected), {
    terminalResult: RequestResult.NotConnected,
    failureCode: 0
  });
  assert.deepEqual(submitted(SubmitResult.NotFound), {
    terminalResult: RequestResult.NotFound,
    failureCode: 0
  });
  for (const [result, terminalResult] of [
    [SubmitResult.Terminated, RequestResult.Terminated],
    [SubmitResult.NotAdmitted, RequestResult.Rejected],
    [SubmitResult.InvalidArgument, RequestResult.InvalidArgument],
    [SubmitResult.InvalidState, RequestResult.InvalidState],
    [SubmitResult.NotSupported, RequestResult.NotSupported],
    [SubmitResult.InternalError, RequestResult.InternalError]
  ]) {
    assert.deepEqual(submitted(result), { terminalResult, failureCode: 0 });
  }

  //  Framework-owned failures. A reply that could not be decoded is a
  //  protocol failure (spec 32-framework-error-model:58-60, 91-92); the
  //  schema terminal-failure-integrity rule makes that pair 104+16.
  assert.deepEqual(requestFailureResult(new ServiceWireProtocolError('Invalid reply parts.')), {
    terminalResult: RequestResult.ProtocolError,
    failureCode: 16
  });
  assert.deepEqual(requestFailureResult(new OperationTimeoutError(1n)), {
    terminalResult: RequestResult.TimedOut,
    failureCode: 0
  });
  assert.deepEqual(requestFailureResult(new OperationCancelledError(1n, 'closed')), {
    terminalResult: RequestResult.NotConnected,
    failureCode: 0
  });
  //  An unclassified exception is a Framework execution failure, not a
  //  transport disconnect.
  assert.deepEqual(requestFailureResult(new Error('socket closed')), {
    terminalResult: RequestResult.InternalError,
    failureCode: 17
  });
});
