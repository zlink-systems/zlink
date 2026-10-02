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

test('tokenless submit refusal is Unavailable while writable completion timeout keeps its deadline', () => {
  const zlink = require('@zlink-systems/zlink');
  const {
    constants: {
      errno: { EAGAIN }
    }
  } = require('node:os');
  const {
    translateBindingResultError
  } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
  const {
    requestFailureResult
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
  const {
    requestResultToPublicErrorKind
  } = require('../../packages/framework/dist/runtime/framework-errors-internal');
  const failure = new zlink.SubmitError(zlink.SubmitResult.Backpressured, EAGAIN);
  const immediate = requestFailureResult(translateBindingResultError(failure, 'submit'));
  const expired = requestFailureResult(translateBindingResultError(failure, 'completion'));

  assert.equal(
    requestResultToPublicErrorKind(immediate.terminalResult),
    framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.notEqual(immediate.terminalResult, RequestResult.Backpressured);
  assert.equal(
    requestResultToPublicErrorKind(expired.terminalResult),
    framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
});

test('raw router and dealer requests preserve the binding failure phase', async (t) => {
  const zlink = require('@zlink-systems/zlink');
  const {
    constants: {
      errno: { EAGAIN }
    }
  } = require('node:os');
  const {
    ZLinkNodeRawBindingPort
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
  const {
    requestFailureResult
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
  const originalCreateRouter = zlink.createRouterSocket;
  const originalCreateDealer = zlink.createDealerSocket;
  let phase = 'submit';
  const withControlledRequest = (socket) => {
    t.mock.method(socket, 'request', () => ({
      message() {
        return this;
      },
      timeout() {
        return this;
      },
      submit() {
        const failure = new zlink.SubmitError(zlink.SubmitResult.Backpressured, EAGAIN);
        if (phase === 'submit') throw failure;
        return { reply: Promise.reject(failure) };
      }
    }));
    return socket;
  };
  t.mock.method(zlink, 'createRouterSocket', (context) =>
    withControlledRequest(originalCreateRouter(context))
  );
  t.mock.method(zlink, 'createDealerSocket', (context) =>
    withControlledRequest(originalCreateDealer(context))
  );

  const context = zlink.createContext();
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  t.after(() => {
    host.close();
    context.close();
  });
  for (const request of [
    () => router.request('target', [Buffer.from('request')], 1000),
    () => dealer.request([Buffer.from('request')], 1000)
  ]) {
    for (const [nextPhase, expected] of [
      ['submit', RequestResult.NotConnected],
      ['completion', RequestResult.Backpressured]
    ]) {
      phase = nextPhase;
      const error = await request().then(
        () => null,
        (failure) => failure
      );
      assert.equal(requestFailureResult(error).terminalResult, expected);
    }
  }
});
