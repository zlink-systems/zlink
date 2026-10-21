const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { createAbortError } = require('../../packages/framework/dist/runtime/abort');

function fixture(submit, codecs) {
  const table = new framework.ZLinkMeshCompletionTable();
  const node = { entrySpot: () => ({ requestToSpot: submit }) };
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined,
    undefined,
    () => ({ meshNode: () => node, meshCompletionTable: () => table }),
    codecs,
    undefined,
    undefined,
    undefined,
    () => false
  );
  return { table, transport };
}

const target = {
  routerChannelId: 'mesh',
  targetNodeRid: 'peer',
  spotId: 'entry',
  spotKind: framework.ZLinkSpotKind.Entry
};

function reply(transport, signal, raw = false) {
  if (raw)
    return transport.requestRawToSpot(
      target,
      framework.ZLinkBufferMessage.fromOwned(Buffer.from('{"packetName":"Probe","value":1}')),
      { timeoutMs: 1000, signal }
    );
  const result = transport.requestToSpot(
    target,
    { value: 1 },
    { packetName: 'Probe', timeoutMs: 1000, signal }
  );
  return result.pending.then(
    (value) => result.decode(value),
    (error) => {
      throw result.mapFailure(error);
    }
  );
}

for (const raw of [false, true]) {
  test(`Spot ${raw ? 'raw ' : ''}request maps rejected typed submit and request failures at the reply boundary`, async () => {
    const { SubmitResult, RequestResult, ZLinkFrameworkErrorKind: Kind } = framework;
    for (const [operation, result, phase, kind] of [
      ['submit', SubmitResult.NotFound, 'submit', Kind.NotFound],
      ['submit', SubmitResult.NotFound, 'completion', Kind.Unavailable],
      ['submit', SubmitResult.NotConnected, 'submit', Kind.Unavailable],
      ['request', RequestResult.NotFound, 'completion', Kind.NotFound],
      ['request', RequestResult.TimedOut, 'completion', Kind.DeadlineExceeded]
    ]) {
      const failure = new framework.ZLinkBackendResultError(operation, result, 0, { phase });
      const { transport, table } = fixture(() => {
        throw failure;
      });
      await assert.rejects(reply(transport, undefined, raw), (error) => {
        assert.ok(error instanceof framework.ZLinkFrameworkException);
        assert.equal(error.kind, kind);
        assert.equal(error.cause, failure);
        return true;
      });
      assert.equal(table.pendingCount, 0);
    }
  });

  test(`Spot ${raw ? 'raw ' : ''}request classifies stale fences and preserves caller failures`, async () => {
    const stale = new framework.ServiceStaleGenerationError('spot', 'entry');
    const staleFixture = fixture(() => {
      throw stale;
    });
    await assert.rejects(reply(staleFixture.transport, undefined, raw), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
      assert.equal(error.cause, stale);
      return true;
    });
    for (const failure of [
      new DOMException('cancelled', 'AbortError'),
      new framework.ZLinkConfigurationException('invalid configuration'),
      new TypeError('invalid argument'),
      new RangeError('invalid argument range'),
      createAbortError(),
      new framework.ZLinkFrameworkException(
        framework.ZLinkFrameworkErrorKind.Rejected,
        'classified'
      )
    ]) {
      const { transport } = fixture(() => {
        throw failure;
      });
      await assert.rejects(reply(transport, undefined, raw), (error) => error === failure);
    }
  });

  test(`Spot ${raw ? 'raw ' : ''}request wraps unknown submission failures with their original cause`, async () => {
    for (const failure of [
      new Error('unclassified failure'),
      new SyntaxError('programming failure'),
      new ReferenceError('programming failure'),
      new EvalError('programming failure'),
      { detail: 'unknown failure' },
      'failure'
    ]) {
      const { transport, table } = fixture(() => { throw failure; });
      await assert.rejects(reply(transport, undefined, raw), (error) => {
        assert.ok(error instanceof framework.ZLinkFrameworkException);
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
        assert.equal(error.cause, failure);
        return true;
      });
      assert.equal(table.pendingCount, 0);
    }
  });

  test(`Spot ${raw ? 'raw ' : ''}request preserves abort reasons while waiting for completion`, async () => {
    const { transport, table } = fixture(() => ({ high: 1n, low: 1n }));
    const controller = new AbortController();
    const pending = reply(transport, controller.signal, raw);
    const reason = new DOMException('cancelled while pending', 'AbortError');
    controller.abort(reason);
    await assert.rejects(pending, (error) => error === reason);
    assert.equal(table.pendingCount, 0);
  });
}

test('Spot request maps preparation failures through the same reply failure mapper', async () => {
  const failure = new framework.ServiceStaleGenerationError('spot', 'preparation');
  const { transport, table } = fixture(
    () => {
      assert.fail('failed encoding must not submit');
    },
    {
      serializers: new Map([
        [
          'application/x-protobuf',
          {
            serialize() {
              throw failure;
            }
          }
        ]
      ])
    }
  );
  await assert.rejects(reply(transport), (error) => {
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    assert.equal(error.cause, failure);
    return true;
  });
  assert.equal(table.pendingCount, 0);
});

for (const path of ['node synchronous', 'node asynchronous', 'channel', 'Spot']) {
  test(`${path} send preserves caller failures and wraps unknown execution failures`, async () => {
    const callerFailures = [
      new TypeError('invalid argument'),
      new RangeError('invalid range'),
      new framework.ZLinkConfigurationException('invalid configuration'),
      createAbortError(),
      new DOMException('cancelled', 'AbortError'),
      new framework.ZLinkFrameworkException(framework.ZLinkFrameworkErrorKind.Rejected, 'classified')
    ];
    for (const failure of [...callerFailures, new Error('unclassified failure'), { detail: 'unknown failure' }]) {
      const fail = () => {
        if (path === 'node asynchronous') return Promise.reject(failure);
        throw failure;
      };
      const node = {
        routingId: 'local',
        sendToNode: fail,
        sendToChannel: fail,
        entrySpot: () => ({ sendToSpot: fail })
      };
      const transport = new framework.ZLinkRuntimeRouteTransport(
        () => undefined, undefined,
        () => ({ meshNode: () => node }),
        undefined, () => true, undefined, undefined, () => false
      );
      await assert.rejects(async () => {
        if (path === 'channel') return transport.submitToChannel('mesh', 'channel', 'Probe', { value: 1 });
        if (path === 'Spot') return transport.sendToSpot(target, { value: 1 }, { packetName: 'Probe' });
        return transport.submit('mesh', 'peer', 'Probe', { value: 1 });
      }, (error) => {
        if (callerFailures.includes(failure)) assert.equal(error, failure);
        else {
          assert.ok(error instanceof framework.ZLinkFrameworkException);
          assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
          assert.equal(error.cause, failure);
        }
        return true;
      });
    }
  });
}

for (const raw of [false, true]) {
  test(`Spot ${raw ? 'raw ' : ''}request retains remote terminal classification and physical submission`, async () => {
    const operationId = { high: 1n, low: 2n };
    const { transport, table } = fixture(() => operationId);
    const pending = reply(transport, undefined, raw);
    table.complete({
      operationId,
      terminalResult: framework.RequestResult.NotFound,
      failureErrno: 0,
      operationKind: 0,
      kindData: null,
      parts: []
    });
    await assert.rejects(pending, (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.NotFound);
      assert.equal(error.physicalSubmission, true);
      return true;
    });
    assert.equal(table.pendingCount, 0);
  });
}
