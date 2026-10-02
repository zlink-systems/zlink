const assert = require('node:assert/strict');
const { test } = require('node:test');

const framework = require('../../packages/framework/dist/internal');

test('plain NotFound uses the C++ general and Actor Join wire representative', () => {
  const reply = framework.internalFrameworkWireReply(
    new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.NotFound,
      'missing target'
    )
  );
  assert.deepEqual(reply, { terminalResult: 102, failureCode: 14 });
  assert.equal(
    framework.wireReplyFailureException(reply.terminalResult, reply.failureCode, 'missing target')
      .kind,
    framework.ZLinkFrameworkErrorKind.NotFound
  );
  assert.equal(
    framework.internalFrameworkWireReply(
      framework.createInternalFrameworkException(
        framework.ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        'missing actor route'
      )
    ).failureCode,
    1
  );
});

test('Unavailable errors round-trip through the generated wire schema', () => {
  const {
    isValidServiceWireTerminalFailure
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
  for (const error of [
    framework.createInternalFrameworkException(
      framework.ZLinkFrameworkInternalErrorKind.ActorRouteUnavailable,
      'owner unavailable'
    ),
    new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.Unavailable,
      'join unavailable'
    )
  ]) {
    const reply = framework.internalFrameworkWireReply(error);
    assert.equal(isValidServiceWireTerminalFailure(reply.terminalResult, reply.failureCode), true);
    assert.equal(
      framework.wireReplyFailureException(reply.terminalResult, reply.failureCode, error.message)
        .kind,
      error.kind
    );
  }
  assert.equal(framework.isCanonicalWireReplyTerminal(105, 42), false);
});

test('public reply and relocation representatives preserve each existing context', () => {
  const codes = [14, 17, 4, 17, 15, 13, 17, 17, 16, 21, 17, 17];
  const actorCodes = [14, 3, 4, 9, 15, 13, 19, 17, 16, 21, 35, 17];
  const spotCodes = [14, 3, 7, 9, 15, 13, 19, 17, 16, 33, 35, 17];
  const {
    isValidServiceWireTerminalFailure
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
  for (let kind = 0; kind < codes.length; ++kind) {
    const reply = framework.internalFrameworkWireReply(
      new framework.ZLinkFrameworkException(kind, 'failure')
    );
    assert.equal(reply.failureCode, codes[kind], `reply kind ${kind}`);
    assert.equal(isValidServiceWireTerminalFailure(reply.terminalResult, reply.failureCode), true);
    assert.equal(framework.frameworkRelocationFailureCode(kind, 'actor'), actorCodes[kind]);
    for (const objectKind of ['instanceSpot', 'userSpot']) {
      assert.equal(framework.frameworkRelocationFailureCode(kind, objectKind), spotCodes[kind]);
    }
  }
  const unknown = new framework.ZLinkFrameworkException(999, 'unknown category');
  assert.deepEqual(framework.internalFrameworkWireReply(unknown), {
    terminalResult: 105,
    failureCode: 17
  });
  assert.equal(framework.frameworkRelocationFailureCode(unknown.kind, 'actor'), 17);
});

test('fine details retain their internal ABI value and generic receive aliases', () => {
  const k = framework.ZLinkFrameworkInternalErrorKind;
  const ordered = [
    k.ActorRouteNotFound,
    k.ActorCreateFailed,
    k.ActorAlreadyExists,
    k.ActorTypeMismatch,
    k.SpotCreateFailed,
    k.SpotRouteNotFound,
    k.SpotTypeMismatch,
    k.ActorSessionNotBound,
    k.HandlerNotFound,
    k.RouteHandlerNotFound,
    k.ActorDispatchHandlerNotFound,
    k.PayloadDecodeFailed,
    k.RouteNotConnected,
    k.RequestTargetNotFound,
    k.RequestRejected,
    k.RequestProtocolError,
    k.RequestFailed,
    k.WorkerQueueFull,
    k.WorkerTimedOut,
    k.WorkerFailed,
    k.ActorLocationStale,
    k.ActorCreateRejected,
    k.ObjectClientNotConfigured,
    k.MeshSelectionRequired,
    k.MeshNotFound,
    k.InvalidConfiguration,
    k.AlreadySubmitted,
    k.ActorGenerationStale,
    k.ActorMoving,
    k.DeadlineExceeded,
    k.PlacementCapacityExhausted,
    k.RoutingIdConflict,
    k.SpotGenerationStale,
    k.SpotMoving,
    k.RelocationDataLost,
    k.SpotIdConflict,
    k.RuntimeShutdown,
    k.RelocationDisabled,
    k.RelocationTargetUnavailable,
    k.RelocationFailed,
    k.InvalidOperation
  ];
  for (let value = 0; value < ordered.length; ++value) {
    const error = framework.createInternalFrameworkException(ordered[value], 'fine failure');
    assert.equal(framework.internalFrameworkErrorCode(error), value);
    const expectedFailure =
      value === 29 ? 0 : value <= 21 || (value >= 32 && value <= 34) ? value + 1 : 17;
    const reply = framework.internalFrameworkWireReply(error);
    assert.equal(reply.failureCode, expectedFailure, `internal reply ${ordered[value]}`);
    if (value === 29) assert.equal(reply.terminalResult, 101);
    assert.equal(
      framework.internalFrameworkErrorKindFromWireFailureCode(value + 1),
      value === 17 ? k.RouteNotConnected : ordered[value]
    );
  }
  assert.equal(
    framework.internalFrameworkErrorCode(
      framework.createInternalFrameworkException(k.ActorRouteUnavailable, 'route')
    ),
    12
  );
  assert.equal(framework.internalFrameworkErrorKindFromWireFailureCode(0), undefined);
  assert.equal(framework.internalFrameworkErrorKindFromWireFailureCode(999), undefined);
});

test('actor and relocation receive aliases preserve fine detail instead of reducing to public kind', () => {
  const k = framework.ZLinkFrameworkInternalErrorKind;
  for (const [code, actor, relocation] of [
    [9, k.RequestTargetNotFound, k.HandlerNotFound],
    [12, k.RequestProtocolError, k.PayloadDecodeFailed],
    [18, k.RouteNotConnected, k.WorkerQueueFull],
    [21, k.ActorLocationStale, k.ActorLocationStale],
    [33, k.ActorGenerationStale, k.SpotGenerationStale],
    [34, k.ActorMoving, k.SpotMoving],
    [35, k.RelocationDataLost, k.RelocationDataLost],
    [22, undefined, k.ActorCreateRejected],
    [999, undefined, undefined]
  ]) {
    assert.equal(framework.internalFrameworkErrorKindFromWireFailureCode(code, 'actor'), actor);
    assert.equal(
      framework.internalFrameworkErrorKindFromWireFailureCode(code, 'relocation'),
      relocation
    );
  }
  assert.equal(framework.internalFrameworkErrorKindFromWireReply(102, 21), k.ActorLocationStale);
  assert.equal(framework.internalFrameworkErrorKindFromWireReply(107, 21), k.ActorLocationStale);
  assert.equal(framework.internalFrameworkErrorKindFromWireReply(106, 21), undefined);
});

test('every internal Framework error produces a canonical stateful wire reply', () => {
  for (const kind of Object.values(framework.ZLinkFrameworkInternalErrorKind)) {
    const error = framework.createInternalFrameworkException(kind, `injected ${kind}`);
    const reply = framework.internalFrameworkWireReply(error);
    assert.equal(
      framework.isCanonicalWireReplyTerminal(reply.terminalResult, reply.failureCode),
      true,
      `${kind} produced ${reply.terminalResult}/${reply.failureCode}`
    );
  }
});

test('stale generation and Actor binding contexts preserve their existing terminal aliases', () => {
  const k = framework.ZLinkFrameworkInternalErrorKind;
  const stale = framework.internalFrameworkWireReply(k.ActorLocationStale, 'stale-generation');
  assert.deepEqual(stale, { terminalResult: 102, failureCode: 21 });
  assert.equal(
    framework.isCanonicalWireReplyTerminal(stale.terminalResult, stale.failureCode),
    true
  );
  assert.equal(
    framework.internalFrameworkErrorKindFromWireReply(stale.terminalResult, stale.failureCode),
    k.ActorLocationStale
  );
  assert.deepEqual(framework.internalFrameworkWireReply(k.ActorLocationStale), {
    terminalResult: 107,
    failureCode: 21
  });
  const binding = framework.internalFrameworkWireReply(k.ActorGenerationStale, 'actor-binding');
  assert.deepEqual(binding, { terminalResult: 111, failureCode: 0 });
  assert.equal(
    framework.isCanonicalWireReplyTerminal(binding.terminalResult, binding.failureCode),
    true
  );
  assert.deepEqual(framework.internalFrameworkWireReply(k.ActorLocationStale, 'actor-binding'), {
    terminalResult: 107,
    failureCode: 21
  });
});

test('stateful wire replies preserve supported detail and mask unsupported detail', () => {
  const missing = framework.internalFrameworkWireReply(
    framework.createInternalFrameworkException(
      framework.ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      'missing actor route'
    )
  );
  assert.deepEqual(missing, { terminalResult: 102, failureCode: 1 });

  const shutdown = framework.internalFrameworkWireReply(
    framework.createInternalFrameworkException(
      framework.ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
      'runtime stopped'
    )
  );
  assert.deepEqual(shutdown, { terminalResult: 105, failureCode: 17 });
});

test('terminal-failure integrity predicate matches the schema exact pairs (round-14)', () => {
  const {
    isValidServiceWireTerminalFailure
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
  //  Valid: success, boundary+none, and exact typed pairs — including the
  //  formerly-rejected defined codes 33/34.
  for (const [terminal, failure] of [
    [0, 0],
    [108, 0],
    [113, 0],
    [101, 0],
    [103, 0],
    [102, 9],
    [105, 17],
    [106, 18],
    [104, 16],
    [107, 33],
    [107, 34],
    [105, 35]
  ]) {
    assert.equal(
      isValidServiceWireTerminalFailure(terminal, failure),
      true,
      `expected valid: ${terminal}+${failure}`
    );
  }
  //  Invalid: mismatched typed pairs, typed terminal with none, boundary with
  //  a code, success with a code, reserved/unknown codes.
  for (const [terminal, failure] of [
    [104, 3],
    [102, 18],
    [102, 17],
    [107, 0],
    [108, 5],
    [0, 3],
    [105, 23],
    [105, 99]
  ]) {
    assert.equal(
      isValidServiceWireTerminalFailure(terminal, failure),
      false,
      `expected invalid: ${terminal}+${failure}`
    );
  }
});

test('m6a reply codec rejects non-canonical terminal/failure pairs as protocol errors (round-14)', () => {
  const {
    encodeReplyHeader,
    decodeReplyHeader,
    ServiceWireProtocolError
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  //  Encode enforces the schema rule.
  assert.throws(() => encodeReplyHeader(1n, 104, 3), ServiceWireProtocolError);
  //  Decode enforces it too: take a legal reply (107+33, formerly rejected by
  //  the old table) and patch its failure code to a mismatched value.
  const legal = encodeReplyHeader(1n, 107, 33);
  assert.deepEqual(decodeReplyHeader(legal).failureCode, 33);
  const patched = Buffer.from(legal);
  patched.writeUInt32BE(18, 5 + 12); // workerQueueFull pairs with 106, not 107
  assert.throws(() => decodeReplyHeader(patched), ServiceWireProtocolError);
});
