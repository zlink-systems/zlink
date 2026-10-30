const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: barrier } = require('node:timers/promises');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  RequestRuntime
} = require('../../packages/framework/dist/runtime/foundation/request-runtime');
const {
  wrapSocket
} = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const {
  submitBindingRequest
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const {
  ZLinkChannelOutboundOperations
} = require('../../packages/framework/dist/runtime/channels/channel-outbound-operations');
const {
  encodeChannelReplyParts,
  decodeChannelReply,
  closeMessages
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkRuntimeMessageFlowOutcome
} = require('../../packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions');
const observe = (promise) =>
  promise.then(
    (value) => ({ value }),
    (error) => ({ error })
  );
const CONTENT_TYPE = 'application/x-q';
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

for (const boundary of ['direct', 'socket', 'ClientServer']) {
  for (const backpressured of [false, true]) {
    for (const terminal of [
      'normal',
      'cancel',
      'deadline',
      'shutdown',
      'shutdown-custom',
      'queued-cancel',
      'physical-first',
      'admission-failure',
      'onAdmitted-failure'
    ]) {
      if (terminal === 'admission-failure' && !backpressured) continue;
      for (const codec of ['valid', 'throwing', 'malformed']) {
        test(`${boundary}/${backpressured ? 'Backpressured' : 'Ok'}/${terminal}/${codec}`, async (t) => {
          let now = 0;
          t.mock.method(performance, 'now', () => now);
          t.mock.timers.enable({ apis: ['setTimeout'] });
          const runtime = new RequestRuntime();
          const controller = new AbortController();
          const reply = deferred();
          const admission = deferred();
          const submitted = deferred();
          const cause = new Error(terminal);
          const codecFailure = new Error('serializer failure');
          let submissions = 0,
            codecCalls = 0,
            settlements = 0,
            fixtureCleanup = 0;
          const events = [];
          const builder = {
            message() {
              return this;
            },
            timeout() {
              return this;
            },
            submit() {
              submissions++;
              submitted.resolve();
              return {
                result: backpressured ? zlink.SubmitResult.Backpressured : zlink.SubmitResult.Ok,
                admitted: admission.promise,
                reply: reply.promise
              };
            }
          };
          const socket = wrapSocket({
            request() {
              return builder;
            },
            close() {}
          });
          const registry = {
            serializers: new Map([
              [
                CONTENT_TYPE,
                {
                  serialize(value) {
                    return framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify(value)));
                  },
                  deserialize() {
                    codecCalls++;
                    if (codec === 'throwing') throw codecFailure;
                    return { answer: 42 };
                  }
                }
              ]
            ])
          };
          const outbound = new ZLinkChannelOutboundOperations(
            {
              async awaitClientDealerForOutbound() {
                return socket;
              },
              selectedClientServerRid() {
                return 'server';
              }
            },
            registry,
            {
              flowCreationEnabled() {
                return false;
              },
              beginOutbound(outcome, result) {
                return {
                  trace() {
                    events.push({ outcome, result });
                  }
                };
              }
            },
            runtime
          );
          const encoded = encodeChannelReplyParts(
            {
              channelName: 'channel',
              messageName: 'reply',
              correlationId: '0123456789abcdef0123456789abcdef'
            },
            { answer: 42 }
          );
          const header = JSON.parse(encoded[0].toString('utf8'));
          header.contentType = CONTENT_TYPE;
          const messages = [
            zlink.Message.from(codec === 'malformed' ? '{' : JSON.stringify(header)),
            zlink.Message.from('answer')
          ];
          const closes = [0, 0];
          const originals = messages.map((message) => message.close);
          const cleanup = messages.map((message) => message.close.bind(message));
          messages.forEach((message, index) => {
            message.close = () => {
              closes[index]++;
              cleanup[index]();
            };
          });
          try {
            const onAdmitted = () => {
              if (terminal === 'onAdmitted-failure') throw cause;
            };
            let pending;
            if (boundary === 'ClientServer') {
              pending = outbound.request(
                'channel',
                'request',
                { request: true },
                50,
                controller.signal,
                undefined,
                onAdmitted
              );
            } else {
              const operation = runtime.begin(50, controller.signal);
              pending = runtime.run(
                operation,
                (receiver) => {
                  if (boundary === 'direct')
                    submitBindingRequest(
                      builder,
                      Buffer.from('request'),
                      50,
                      undefined,
                      onAdmitted,
                      operation,
                      receiver
                    );
                  else socket.request(Buffer.from('request'), 50, onAdmitted, operation, receiver);
                },
                (raw) => {
                  try {
                    return decodeChannelReply(raw, registry);
                  } finally {
                    closeMessages(raw);
                  }
                },
                (error) => error
              );
              assert.equal(pending, operation.promise);
            }
            const caller = observe(pending);
            void caller.then(() => {
              settlements++;
            });
            await submitted.promise;
            if (terminal === 'admission-failure') admission.reject(cause);
            else admission.resolve();
            if (terminal === 'queued-cancel') reply.resolve(messages);
            if (terminal === 'cancel' || terminal === 'queued-cancel') controller.abort(cause);
            if (terminal === 'shutdown') runtime.close();
            if (terminal === 'shutdown-custom') runtime.close(cause);
            if (terminal === 'deadline') {
              now += 50;
              t.mock.timers.tick(50);
            }
            const early = !['normal', 'physical-first'].includes(terminal);
            let before;
            if (early) {
              before = await caller;
              assert.equal(runtime.registry.size, 0);
            }
            if (terminal !== 'queued-cancel') reply.resolve(messages);
            const result = await caller;
            if (terminal === 'physical-first') controller.abort(cause);
            await barrier();
            const observedCloses = [...closes];
            const observedCodecs = codecCalls;
            const terminalTraceCount = events.filter(
              (event) => event.outcome === ZLinkRuntimeMessageFlowOutcome.ReplyReceived
            ).length;
            assert.equal(submissions, 1);
            assert.equal(settlements, 1);
            assert.equal(runtime.registry.size, 0);
            assert.deepEqual(observedCloses, [1, 1]);
            assert.equal(observedCodecs, codec === 'malformed' ? 0 : 1);
            assert.equal(fixtureCleanup, 0);
            if (early) {
              assert.equal(result, before);
              if (terminal === 'deadline')
                assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
              else if (terminal === 'shutdown')
                assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
              else assert.equal(result.error, cause);
            } else if (codec === 'valid') assert.deepEqual(result.value, { answer: 42 });
            else if (codec === 'throwing') assert.equal(result.error, codecFailure);
            else assert.ok(result.error);
            if (boundary === 'ClientServer') {
              assert.equal(outbound.pendingRequestCount('channel'), 0);
              assert.equal(terminalTraceCount, 1);
            }
          } finally {
            messages.forEach((message, index) => {
              message.close = originals[index];
              if (closes[index] === 0) {
                fixtureCleanup++;
                cleanup[index]();
              }
            });
            runtime.close();
            socket.close();
          }
        });
      }
    }
  }
}

for (const terminal of ['deadline', 'shutdown']) {
  test(`ClientServer readiness/${terminal}/submit0`, async (t) => {
    let now = 0;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const runtime = new RequestRuntime();
    let submits = 0,
      aborted = 0;
    const waiting = deferred();
    const cause = new Error('shutdown');
    const outbound = new ZLinkChannelOutboundOperations(
      {
        awaitClientDealerForOutbound(_channel, signal) {
          signal.addEventListener(
            'abort',
            () => {
              aborted++;
              waiting.reject(signal.reason);
            },
            { once: true }
          );
          return waiting.promise;
        },
        selectedClientServerRid() {
          submits++;
          return 'unexpected';
        }
      },
      undefined,
      {
        flowCreationEnabled() {
          return false;
        },
        beginOutbound() {}
      },
      runtime
    );
    const caller = observe(outbound.request('channel', 'request', {}, 50));
    if (terminal === 'deadline') {
      now += 50;
      t.mock.timers.tick(50);
    } else runtime.close(cause);
    const result = await caller;
    await barrier();
    assert.equal(submits, 0);
    assert.equal(aborted, 1);
    assert.equal(runtime.registry.size, 0);
    if (terminal === 'deadline')
      assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
    else assert.equal(result.error, cause);
    runtime.close();
  });
}

for (const cancelled of [false, true]) {
  test(`actual mesh completion observer directly retains prepared receiver/cancel=${cancelled}`, async () => {
    const {
      ZLinkMeshCompletionTable
    } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
    const {
      ZLinkNodeRawMeshBackend
    } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
    const {
      ServiceMailbox
    } = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
    const runtime = new RequestRuntime();
    const controller = new AbortController();
    const entry = runtime.begin(1000, controller.signal);
    const physical = deferred();
    const table = new ZLinkMeshCompletionTable();
    let decoded = 0,
      notifications = 0;
    const raw = {
      mailbox: new ServiceMailbox(),
      requestToNode(_target, _parts, _timeout, _admitted, observer, kind, pending) {
        void physical.promise.then((value) => {
          notifications++;
          observer.operationCompleted(pending.id, value, kind);
        });
        return pending;
      },
      close() {
        this.mailbox.close();
      }
    };
    const backend = new ZLinkNodeRawMeshBackend(
      'mesh',
      'local',
      {},
      undefined,
      undefined,
      undefined,
      undefined,
      { raw }
    );
    const caller = observe(
      runtime.run(
        entry,
        (receiver) => {
          table.submit(
            (completion) =>
              backend.requestToNode(
                'peer',
                [Buffer.from('request')],
                { requestOperation: entry },
                completion
              ),
            undefined,
            entry,
            receiver
          );
        },
        (value) => {
          decoded++;
          return value.terminalResult;
        },
        (error) => error
      )
    );
    const reason = new Error('cancel');
    if (cancelled) {
      controller.abort(reason);
      assert.equal((await caller).error, reason);
      assert.equal(runtime.registry.size, 0);
    }
    physical.resolve({ terminalResult: 0, failureCode: 0 });
    await barrier();
    const result = await caller;
    assert.equal(decoded, 1);
    assert.equal(notifications, 1);
    assert.equal(table.pendingCount, 0);
    assert.equal(runtime.registry.size, 0);
    if (cancelled) assert.equal(result.error, reason);
    else assert.equal(result.value, 0);
    runtime.close();
    table.dispose();
    raw.close();
  });
}

for (const raw of [false, true]) {
  for (const cancelled of [false, true]) {
    test(`actual Spot wrapper/${raw ? 'raw' : 'typed'}/cancel=${cancelled}`, async () => {
      const {
        ZLinkSpotNodeRouteTransport
      } = require('../../packages/framework/dist/runtime/channels/spot-node-route-transport');
      const runtime = new RequestRuntime();
      const physical = deferred();
      const controller = new AbortController();
      const socket = wrapSocket({
        requestToSpot() {
          return {
            message() {
              return this;
            },
            timeout() {
              return this;
            },
            submit() {
              return {
                result: zlink.SubmitResult.Ok,
                admitted: Promise.resolve(),
                reply: physical.promise
              };
            }
          };
        },
        close() {}
      });
      const transport = new ZLinkSpotNodeRouteTransport(
        { requestTimeoutMs: 1000 },
        {
          spotNodeRouter() {
            return socket;
          }
        },
        runtime
      );
      const target = { routerChannelId: 'mesh', targetNodeRid: 'peer', spotId: 'spot' };
      const request = zlink.Message.from('request');
      const encoded = encodeChannelReplyParts(
        {
          channelName: 'channel',
          messageName: 'reply',
          correlationId: '0123456789abcdef0123456789abcdef'
        },
        { answer: 42 }
      );
      const messages = encoded.map((part) => zlink.Message.from(part));
      const closes = messages.map(() => 0);
      const originals = messages.map((message) => message.close);
      messages.forEach((message, index) => {
        const close = message.close.bind(message);
        message.close = () => {
          closes[index]++;
          close();
        };
      });
      try {
        const caller = observe(
          raw
            ? transport.requestRaw(target, request, 1000, controller.signal)
            : transport.request(target, [request], undefined, 1000, controller.signal)
        );
        const reason = new Error('cancel');
        if (cancelled) {
          controller.abort(reason);
          assert.equal((await caller).error, reason);
        }
        physical.resolve(messages);
        const result = await caller;
        await barrier();
        if (raw && !cancelled) {
          assert.deepEqual(closes, [0, 0]);
          closeMessages(result.value);
        }
        assert.deepEqual(closes, [1, 1]);
        if (cancelled) assert.equal(result.error, reason);
        else if (!raw) assert.deepEqual(result.value, { answer: 42 });
      } finally {
        messages.forEach((message, index) => {
          message.close = originals[index];
          if (closes[index] === 0) message.close();
        });
        if (raw) request.close();
        runtime.close();
        socket.close();
      }
    });
  }
}

test('standalone wrapSocket supplies its own raw receiver before submit', async () => {
  let submits = 0;
  const messages = [zlink.Message.from('reply')];
  const socket = wrapSocket({
    request() {
      return {
        message() {
          return this;
        },
        submit() {
          submits++;
          return {
            result: zlink.SubmitResult.Ok,
            admitted: Promise.resolve(),
            reply: Promise.resolve(messages)
          };
        }
      };
    },
    close() {}
  });
  const result = await socket.request(Buffer.from('request'));
  assert.equal(submits, 1);
  assert.equal(result, messages);
  closeMessages(result);
  await socket.dispose();
});

test('ClientServer pre-aborted custom reason keeps identity and records one terminal without readiness or submit', async () => {
  const runtime = new RequestRuntime();
  const reason = new Error('already cancelled');
  const events = [];
  let readiness = 0;
  const outbound = new ZLinkChannelOutboundOperations(
    {
      awaitClientDealerForOutbound() {
        readiness++;
        throw new Error('must not start readiness');
      }
    },
    undefined,
    {
      flowCreationEnabled() {
        return false;
      },
      beginOutbound(outcome, result) {
        return {
          trace() {
            events.push({ outcome, result });
          }
        };
      }
    },
    runtime
  );
  const result = await observe(
    outbound.request('channel', 'request', {}, 50, AbortSignal.abort(reason))
  );
  assert.equal(result.error, reason);
  assert.equal(readiness, 0);
  assert.equal(runtime.registry.size, 0);
  assert.equal(outbound.pendingRequestCount('channel'), 0);
  assert.equal(events.length, 1);
  assert.equal(events[0].outcome, ZLinkRuntimeMessageFlowOutcome.ReplyReceived);
  assert.equal(events[0].result, 'cancelled');
  runtime.close();
});

for (const mapper of ['return', 'throw']) {
  for (const terminal of ['physical', 'cancel', 'deadline', 'shutdown', 'decode']) {
    test(`failure owner/${terminal}/${mapper}/physical-map1-logical-map0-terminal1`, async (t) => {
      const runtime = new RequestRuntime();
      const controller = new AbortController();
      const entry = runtime.begin(1000, controller.signal);
      const cause = new Error(terminal);
      const mapped = new Error('mapped');
      let receiver,
        mappings = 0,
        settlements = 0,
        codecs = 0;
      const caller = observe(
        runtime.run(
          entry,
          (value) => {
            receiver = value;
          },
          () => {
            codecs++;
            if (terminal === 'decode') throw cause;
            return 'late';
          },
          (error) => {
            mappings++;
            if (terminal === 'deadline')
              assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
            else assert.equal(error, cause);
            if (mapper === 'throw') throw mapped;
            return mapped;
          }
        )
      );
      void caller.then(() => {
        settlements++;
      });
      try {
        if (terminal === 'physical') receiver.operationFailed(entry.id, cause, undefined);
        if (terminal === 'cancel') controller.abort(cause);
        if (terminal === 'deadline') runtime.registry.expire(entry.deadlineMs, Infinity);
        if (terminal === 'shutdown') runtime.close(cause);
        if (terminal === 'decode') receiver.operationCompleted(entry.id, 'raw', undefined);
        const result = await caller;
        if (terminal === 'physical') assert.equal(result.error, mapped);
        else if (terminal === 'deadline')
          assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
        else assert.equal(result.error, cause);
        assert.equal(mappings, terminal === 'physical' ? 1 : 0);
        receiver.operationCompleted(entry.id, 'late', undefined);
        await barrier();
        assert.equal(mappings, terminal === 'physical' ? 1 : 0);
        assert.equal(settlements, 1);
        assert.equal(codecs, terminal === 'decode' ? 2 : 1);
        assert.equal(runtime.registry.size, 0);
      } finally {
        runtime.close();
      }
    });
  }
}

test('ClientServer rejected caller is not reinjected into physical receiver', async (t) => {
  const runtime = new RequestRuntime();
  const originalRun = runtime.run.bind(runtime);
  let mappings = 0,
    failures = 0,
    traces = 0;
  t.mock.method(runtime, 'run', (entry, start, decode, mapFailure) =>
    originalRun(
      entry,
      (receiver) => {
        start({
          operationCompleted: (...args) => receiver.operationCompleted(...args),
          operationFailed: (...args) => {
            failures++;
            receiver.operationFailed(...args);
          }
        });
      },
      decode,
      (error) => {
        mappings++;
        return mapFailure(error);
      }
    )
  );
  const cause = new Error('physical');
  const socket = {
    request(_parts, _timeout, _admitted, entry, receiver) {
      receiver.operationFailed(entry.id, cause, undefined);
    }
  };
  const outbound = new ZLinkChannelOutboundOperations(
    {
      async awaitClientDealerForOutbound() {
        return socket;
      },
      selectedClientServerRid() {
        return 'server';
      }
    },
    undefined,
    {
      flowCreationEnabled() {
        return false;
      },
      beginOutbound(outcome) {
        return {
          trace() {
            if (outcome === ZLinkRuntimeMessageFlowOutcome.ReplyReceived) traces++;
          }
        };
      }
    },
    runtime
  );
  try {
    assert.equal((await observe(outbound.request('channel', 'request', {}, 1000))).error, cause);
    await barrier();
    assert.equal(failures, 1);
    assert.equal(mappings, 1);
    assert.equal(traces, 1);
    assert.equal(outbound.pendingRequestCount('channel'), 0);
  } finally {
    runtime.close();
  }
});

for (const terminal of ['value', 'failure', 'already-terminal']) {
  test(`mapZLinkRequestReply keeps explicit mapping with operation/${terminal}`, async () => {
    const {
      mapZLinkRequestReply
    } = require('../../packages/framework/dist/runtime/messaging/request-result');
    const runtime = new RequestRuntime();
    const entry = runtime.begin(1000);
    let decoded = 0,
      mapped = 0;
    const cause = new Error('physical');
    const failure = new Error('mapped');
    try {
      if (terminal === 'already-terminal') entry.complete('raw');
      const caller = observe(
        mapZLinkRequestReply({
          operation: entry,
          pending: entry.promise,
          decode(value) {
            decoded++;
            assert.equal(value, 'raw');
            return 'typed';
          },
          mapFailure(error) {
            mapped++;
            assert.equal(error, cause);
            return failure;
          }
        })
      );
      if (terminal === 'value') entry.complete('raw');
      if (terminal === 'failure') entry.fail(cause);
      const result = await caller;
      if (terminal === 'failure') {
        assert.equal(result.error, failure);
        assert.equal(mapped, 1);
        assert.equal(decoded, 0);
      } else {
        assert.equal(result.value, 'typed');
        assert.equal(decoded, 1);
        assert.equal(mapped, 0);
      }
    } finally {
      runtime.close();
    }
  });
}

test('bindingRequestReply metadata failure preserves physical codec, close and operation id', async () => {
  const {
    bindingRequestReply
  } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
  const runtime = new RequestRuntime();
  // Use the second actual entry so a constant sentinel cannot satisfy the identity assertion.
  runtime.begin(1000).complete(undefined);
  const entry = runtime.begin(1000);
  const reason = new Error('metadata');
  const messages = [zlink.Message.from('reply')];
  const original = messages[0].close;
  const unhandled = [];
  const onUnhandled = (error) => unhandled.push(error);
  let codecs = 0,
    closes = 0,
    failures = 0;
  messages[0].close = function () {
    closes++;
    original.call(this);
  };
  process.on('unhandledRejection', onUnhandled);
  try {
    const caller = observe(
      runtime.run(
        entry,
        (receiver) => {
          bindingRequestReply(
            {
              result: zlink.SubmitResult.Ok,
              admitted: Promise.resolve(),
              reply: Promise.resolve(messages)
            },
            {
              operationCompleted(id, value, context) {
                assert.equal(id, entry.id);
                assert.equal(context, undefined);
                receiver.operationCompleted(id, value, context);
              },
              operationFailed(id, error, context) {
                failures++;
                assert.equal(id, entry.id);
                receiver.operationFailed(id, error, context);
              }
            },
            entry,
            () => {
              throw reason;
            }
          );
        },
        (parts) => {
          try {
            codecs++;
            return parts[0].data().toString();
          } finally {
            closeMessages(parts);
          }
        },
        (error) => error
      )
    );
    assert.equal((await caller).error, reason);
    await barrier();
    assert.equal(failures, 1);
    assert.equal(codecs, 1);
    assert.equal(closes, 1);
    assert.deepEqual(unhandled, []);
  } finally {
    process.off('unhandledRejection', onUnhandled);
    messages[0].close = original;
    if (closes === 0) messages[0].close();
    runtime.close();
  }
});

for (const boundary of ['source', 'router', 'bridge']) {
  for (const raw of [false, true]) {
    for (const terminal of ['normal', 'cancel', 'deadline']) {
      test(`Spot constructor/${boundary}/${raw ? 'raw' : 'typed'}/${terminal}`, async (t) => {
        const {
          ZLinkSourceSpotRouter
        } = require('../../packages/framework/dist/runtime/channels/source-spot-router');
        const {
          ZLinkRouterSocketSpotRouteTransport
        } = require('../../packages/framework/dist/runtime/channels/router-socket-spot-route-transport');
        const {
          ZLinkSpotRouteBridgeTransport
        } = require('../../packages/framework/dist/runtime/channels/spot-route-bridge-transport');
        const {
          SPOT_DIRECT_ENVELOPE
        } = require('../../packages/framework/dist/runtime/channels/spot-direct-envelope');
        let now = 0;
        t.mock.method(performance, 'now', () => now);
        t.mock.timers.enable({ apis: ['setTimeout'] });
        const runtime = new RequestRuntime();
        const controller = new AbortController();
        const physical = deferred();
        let codecs = 0,
          submissions = 0,
          bridgeCaller;
        const socket = wrapSocket({
          requestToSpot() {
            return {
              message() {
                return this;
              },
              timeout() {
                return this;
              },
              submit() {
                submissions++;
                return {
                  result: zlink.SubmitResult.Ok,
                  admitted: Promise.resolve(),
                  reply: physical.promise
                };
              }
            };
          },
          close() {}
        });
        // No Node production bridge exists. This cooperative builder consumes the receiver
        // and deliberately returns the caller Promise, which must never be observed as physical.
        const bridge = {
          request() {
            const parts = [];
            return {
              message(part) {
                parts.push(part);
                return this;
              },
              timeout() {
                return this;
              },
              submit(admitted, entry, receiver) {
                assert.ok(receiver);
                socket.requestToSpot('peer', 'spot', parts, 50, admitted, entry, receiver);
                bridgeCaller = entry.promise;
                return bridgeCaller;
              }
            };
          }
        };
        const target = { routerChannelId: 'channel', targetNodeRid: 'peer', spotId: 'spot' };
        const request = zlink.Message.from('request');
        const codecRegistry = {
          serializers: new Map([
            [
              CONTENT_TYPE,
              {
                deserialize() {
                  codecs++;
                  return { answer: 42 };
                }
              }
            ]
          ])
        };
        let parts;
        if (boundary === 'source' && !raw)
          parts = [
            Buffer.from(
              JSON.stringify({ marker: SPOT_DIRECT_ENVELOPE, ok: true, response: { answer: 42 } })
            )
          ];
        else {
          parts = encodeChannelReplyParts(
            {
              channelName: 'channel',
              messageName: 'reply',
              correlationId: '0123456789abcdef0123456789abcdef'
            },
            { answer: 42 }
          );
          const header = JSON.parse(parts[0].toString());
          header.contentType = CONTENT_TYPE;
          parts[0] = Buffer.from(JSON.stringify(header));
        }
        const messages = parts.map((part) => zlink.Message.from(part));
        const originals = messages.map((message) => ({ close: message.close, data: message.data }));
        const closes = messages.map(() => 0);
        messages.forEach((message, index) => {
          message.close = function () {
            closes[index]++;
            originals[index].close.call(this);
          };
          if (boundary === 'source' && !raw)
            message.data = function () {
              codecs++;
              return originals[index].data.call(this);
            };
        });
        const reason = new Error('cancel');
        try {
          let pending;
          if (boundary === 'source') {
            const transport = new ZLinkSourceSpotRouter(50, runtime);
            pending = raw
              ? transport.requestRaw(socket, target, request, 50, controller.signal)
              : transport.request(socket, target, 'request', {}, 50, controller.signal);
          } else if (boundary === 'router') {
            const transport = new ZLinkRouterSocketSpotRouteTransport(
              {
                routeRouter() {
                  return socket;
                }
              },
              runtime
            );
            pending = raw
              ? transport.requestRaw(target, request, 50, controller.signal)
              : transport.request(target, [request], codecRegistry, 0, 50, controller.signal);
          } else {
            const transport = new ZLinkSpotRouteBridgeTransport(
              new Map([['channel', bridge]]),
              50,
              runtime
            );
            pending = raw
              ? transport.requestRaw(target, request, 50, controller.signal)
              : transport.request(target, [request], codecRegistry, 50, controller.signal);
            assert.ok(bridgeCaller);
          }
          const caller = observe(pending);
          if (terminal === 'cancel') controller.abort(reason);
          if (terminal === 'deadline') {
            now += 50;
            t.mock.timers.tick(50);
          }
          if (terminal !== 'normal') {
            const result = await caller;
            if (terminal === 'cancel') assert.equal(result.error, reason);
            else
              assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
          }
          physical.resolve(messages);
          const result = await caller;
          await barrier();
          if (terminal === 'normal') {
            if (raw) {
              assert.deepEqual(
                closes,
                messages.map(() => 0)
              );
              closeMessages(result.value);
            } else assert.deepEqual(result.value, { answer: 42 });
          }
          assert.equal(submissions, 1);
          assert.equal(codecs, raw ? 0 : 1);
          assert.deepEqual(
            closes,
            messages.map(() => 1)
          );
          assert.equal(runtime.registry.size, 0);
        } finally {
          messages.forEach((message, index) => {
            message.close = originals[index].close;
            message.data = originals[index].data;
            if (closes[index] === 0) message.close();
          });
          if (raw || boundary === 'source') request.close();
          runtime.close();
          socket.close();
        }
      });
    }
  }
}

test('SourceSpot #2911 begins typed requests before payload serialization', async (t) => {
  const {
    ZLinkSourceSpotRouter
  } = require('../../packages/framework/dist/runtime/channels/source-spot-router');
  const {
    SPOT_DIRECT_ENVELOPE
  } = require('../../packages/framework/dist/runtime/channels/spot-direct-envelope');
  let now = 0;
  const clock = t.mock.method(performance, 'now', () => now);
  const target = {
    routerChannelId: 'channel',
    targetNodeRid: Buffer.from('node'),
    spotId: Buffer.from('spot')
  };
  const rows = [];

  async function runCase(
    name,
    { existing = false, terminal = false, aborted = false, throwing = false, advance = 20 } = {}
  ) {
    now = 0;
    const runtime = new RequestRuntime();
    const transport = new ZLinkSourceSpotRouter(50, runtime);
    const controller = new AbortController();
    const customReason = new Error(`custom-${name}`);
    const encodingError = new Error(`encoding-${name}`);
    const existingEntry = existing ? runtime.begin(50) : undefined;
    if (terminal) existingEntry.fail(customReason);
    if (aborted) controller.abort(customReason);
    let getterCalls = 0,
      sourceCalls = 0,
      submits = 0,
      nativeTimeout;
    let seenEntry, registryAtSource;
    let replyDataCalls = 0,
      replyCloseCalls = 0,
      replyMessage;
    const physical = deferred();
    const adapterPromises = [];
    const builder = {
      message(part) {
        requestPartsSeen.push(Buffer.isBuffer(part));
        return this;
      },
      timeout(ms) {
        nativeTimeout = ms;
        return this;
      },
      submit() {
        submits++;
        return {
          result: zlink.SubmitResult.Ok,
          admitted: Promise.resolve(),
          reply: physical.promise
        };
      }
    };
    const requestPartsSeen = [];
    const native = {
      requestToSpot() {
        return builder;
      },
      close() {}
    };
    const adapter = wrapSocket(native);
    const backend = {
      requestToSpot(...args) {
        sourceCalls++;
        seenEntry = args[5];
        registryAtSource = runtime.registry.size;
        const pending = adapter.requestToSpot(...args);
        adapterPromises.push(observe(pending));
        return pending;
      }
    };
    const payload = {
      get value() {
        getterCalls++;
        now += advance;
        if (throwing) throw encodingError;
        return 'payload';
      }
    };
    const entryObserved = existingEntry ? observe(existingEntry.promise) : undefined;
    let outcome, entryOutcome, sizeBeforeCleanup, replyValue;
    let originalReplyData, originalReplyClose;
    try {
      const pending = observe(
        transport.request(
          backend,
          target,
          'packet',
          payload,
          50,
          aborted ? controller.signal : undefined,
          undefined,
          existingEntry
        )
      );
      if (submits > 0) {
        replyMessage = zlink.Message.from(
          Buffer.from(
            JSON.stringify({
              marker: SPOT_DIRECT_ENVELOPE,
              ok: true,
              response: { answer: 42 }
            })
          )
        );
        originalReplyData = replyMessage.data;
        originalReplyClose = replyMessage.close;
        replyMessage.data = function (...dataArgs) {
          replyDataCalls++;
          return originalReplyData.apply(this, dataArgs);
        };
        replyMessage.close = function (...closeArgs) {
          replyCloseCalls++;
          return originalReplyClose.apply(this, closeArgs);
        };
        physical.resolve([replyMessage]);
      }
      outcome = await pending;
      await Promise.all(adapterPromises);
      if (entryObserved) entryOutcome = await entryObserved;
      if (outcome.error === undefined) replyValue = outcome.value;
      sizeBeforeCleanup = runtime.registry.size;
    } finally {
      runtime.close();
      await adapter.dispose();
      if (replyMessage) {
        replyMessage.data = originalReplyData;
        replyMessage.close = originalReplyClose;
        if (replyCloseCalls === 0) originalReplyClose.call(replyMessage);
      }
    }
    const identify = (error) => {
      if (error === undefined) return undefined;
      if (error === customReason) return 'customReason';
      if (error === encodingError) return 'encodingError';
      if (error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded)
        return 'DeadlineExceeded';
      return error.constructor?.name ?? String(error);
    };
    rows.push({
      name,
      getterCalls,
      now,
      sourceCalls,
      submits,
      nativeTimeout,
      deadlineMs: seenEntry?.deadlineMs ?? existingEntry?.deadlineMs,
      seenEntryId: seenEntry?.id,
      existingEntryId: existingEntry?.id,
      reusedExisting: existingEntry ? seenEntry === existingEntry : false,
      registryAtSource,
      registryBeforeCleanup: sizeBeforeCleanup,
      requestPartsSeen,
      error: identify(outcome?.error),
      value: replyValue,
      existingError: identify(entryOutcome?.error),
      replyDataCalls,
      replyCloseCalls
    });
  }

  try {
    await runCase('new');
    await runCase('existing', { existing: true });
    await runCase('pre-abort-normal', { aborted: true });
    await runCase('pre-abort-throwing', { aborted: true, throwing: true });
    await runCase('existing-terminal', { existing: true, terminal: true });
    await runCase('active-throwing', { throwing: true });
    await runCase('deadline-50', { advance: 50 });
    await runCase('deadline-60', { advance: 60 });
    t.diagnostic(
      JSON.stringify(rows, (_key, value) => (typeof value === 'bigint' ? value.toString() : value))
    );
    const select = ({
      name,
      getterCalls,
      now,
      sourceCalls,
      submits,
      nativeTimeout,
      deadlineMs,
      seenEntryId,
      existingEntryId,
      reusedExisting,
      registryAtSource,
      registryBeforeCleanup,
      error,
      value,
      existingError,
      replyDataCalls,
      replyCloseCalls
    }) => ({
      name,
      getterCalls,
      now,
      sourceCalls,
      submits,
      nativeTimeout,
      deadlineMs,
      seenEntryId,
      existingEntryId,
      reusedExisting,
      registryAtSource,
      registryBeforeCleanup,
      error,
      value,
      existingError,
      replyDataCalls,
      replyCloseCalls
    });
    assert.deepEqual(rows.map(select), [
      {
        name: 'new',
        getterCalls: 1,
        now: 20,
        sourceCalls: 1,
        submits: 1,
        nativeTimeout: 30,
        deadlineMs: 50,
        seenEntryId: rows[0].seenEntryId,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: 1,
        registryBeforeCleanup: 0,
        error: undefined,
        value: { answer: 42 },
        existingError: undefined,
        replyDataCalls: 1,
        replyCloseCalls: 1
      },
      {
        name: 'existing',
        getterCalls: 1,
        now: 20,
        sourceCalls: 1,
        submits: 1,
        nativeTimeout: 30,
        deadlineMs: 50,
        seenEntryId: rows[1].existingEntryId,
        existingEntryId: rows[1].existingEntryId,
        reusedExisting: true,
        registryAtSource: 1,
        registryBeforeCleanup: 0,
        error: undefined,
        value: { answer: 42 },
        existingError: undefined,
        replyDataCalls: 1,
        replyCloseCalls: 1
      },
      {
        name: 'pre-abort-normal',
        getterCalls: 0,
        now: 0,
        sourceCalls: 0,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: undefined,
        seenEntryId: undefined,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: undefined,
        registryBeforeCleanup: 0,
        error: 'customReason',
        value: undefined,
        existingError: undefined,
        replyDataCalls: 0,
        replyCloseCalls: 0
      },
      {
        name: 'pre-abort-throwing',
        getterCalls: 0,
        now: 0,
        sourceCalls: 0,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: undefined,
        seenEntryId: undefined,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: undefined,
        registryBeforeCleanup: 0,
        error: 'customReason',
        value: undefined,
        existingError: undefined,
        replyDataCalls: 0,
        replyCloseCalls: 0
      },
      {
        name: 'existing-terminal',
        getterCalls: 0,
        now: 0,
        sourceCalls: 0,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: 50,
        seenEntryId: undefined,
        existingEntryId: rows[4].existingEntryId,
        reusedExisting: false,
        registryAtSource: undefined,
        registryBeforeCleanup: 0,
        error: 'customReason',
        value: undefined,
        existingError: 'customReason',
        replyDataCalls: 0,
        replyCloseCalls: 0
      },
      {
        name: 'active-throwing',
        getterCalls: 1,
        now: 20,
        sourceCalls: 0,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: undefined,
        seenEntryId: undefined,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: undefined,
        registryBeforeCleanup: 0,
        error: 'encodingError',
        value: undefined,
        existingError: undefined,
        replyDataCalls: 0,
        replyCloseCalls: 0
      },
      {
        name: 'deadline-50',
        getterCalls: 1,
        now: 50,
        sourceCalls: 1,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: 50,
        seenEntryId: rows[6].seenEntryId,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: 1,
        registryBeforeCleanup: 0,
        error: 'DeadlineExceeded',
        value: undefined,
        existingError: undefined,
        replyDataCalls: 0,
        replyCloseCalls: 0
      },
      {
        name: 'deadline-60',
        getterCalls: 1,
        now: 60,
        sourceCalls: 1,
        submits: 0,
        nativeTimeout: undefined,
        deadlineMs: 50,
        seenEntryId: rows[7].seenEntryId,
        existingEntryId: undefined,
        reusedExisting: false,
        registryAtSource: 1,
        registryBeforeCleanup: 0,
        error: 'DeadlineExceeded',
        value: undefined,
        existingError: undefined,
        replyDataCalls: 0,
        replyCloseCalls: 0
      }
    ]);
    assert.deepEqual(rows[0].requestPartsSeen, [true]);
  } finally {
    clock.mock.restore();
  }
});

test('throwing failure mapper retains Request terminal classification', async () => {
  const {
    OperationTimeoutError
  } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
  const runtime = new RequestRuntime();
  const entry = runtime.begin(1000);
  let receiver;
  const caller = observe(
    runtime.run(
      entry,
      (value) => {
        receiver = value;
      },
      (value) => value,
      () => {
        throw new OperationTimeoutError(entry.id);
      }
    )
  );
  try {
    receiver.operationFailed(entry.id, new Error('physical'), undefined);
    assert.equal((await caller).error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  } finally {
    runtime.close();
  }
});

for (const publicResult of ['submit', 'yield']) {
  for (const backpressured of [false, true]) {
    for (const terminal of [
      'normal',
      'cancel',
      'deadline',
      'shutdown',
      'shutdown-custom',
      'codec',
      'physical',
      'submit-failure',
      'malformed',
      'after-abort',
      'admission-failure',
      'cancel-codec',
      'deadline-codec',
      'shutdown-codec',
      'shutdown-custom-codec',
      'admission-failure-codec',
      'cancel-malformed'
    ]) {
      if (!backpressured && terminal.startsWith('admission-failure')) continue;
      test(`actual Dealer public/${publicResult}/${backpressured ? 'Backpressured' : 'Ok'}/${terminal}/preserves error owner and late cleanup`, async (t) => {
        const {
          ZLinkDealerChannelClientTransport
        } = require('../../packages/framework/dist/runtime/backend/node/node-dealer-channel-client-transport');
        const {
          createZLinkRequest,
          mapZLinkRequestReply
        } = require('../../packages/framework/dist/runtime/messaging/request-result');
        const runtime = new RequestRuntime();
        const controller = new AbortController();
        const terminalMode = terminal.replace(/-(codec|malformed)$/, '');
        const nativeReply = deferred();
        const admission = deferred();
        const reason = new Error(terminal);
        const codecFailure = new Error('late codec');
        let admitted = 0,
          codecs = 0,
          bodyCalls = 0,
          settlements = 0;
        const builder = {
          message() {
            return this;
          },
          timeout() {
            return this;
          },
          submit() {
            if (terminal === 'submit-failure') throw reason;
            return {
              result: backpressured ? zlink.SubmitResult.Backpressured : zlink.SubmitResult.Ok,
              admitted: admission.promise,
              reply: nativeReply.promise
            };
          }
        };
        const transport = new ZLinkDealerChannelClientTransport(
          {
            request() {
              return builder;
            }
          },
          undefined,
          runtime
        );
        const request = zlink.Message.from('{}');
        const parts = encodeChannelReplyParts(
          {
            channelName: 'channel',
            messageName: 'reply',
            correlationId: '0123456789abcdef0123456789abcdef'
          },
          { answer: 42 }
        ).map((value) => zlink.Message.from(value));
        const closes = parts.map(() => 0);
        const originalClose = parts.map((part) => part.close);
        const originalData = parts[0].data;
        const originalBodyData = parts[1].data;
        const close = parts.map((part) => part.close.bind(part));
        const data = parts[0].data.bind(parts[0]);
        parts[0].data = () => {
          codecs++;
          if (terminal === 'codec') throw reason;
          if (terminal.endsWith('-codec')) throw codecFailure;
          return terminal === 'malformed' || terminal.endsWith('-malformed')
            ? Buffer.from('{')
            : data();
        };
        parts[1].data = function () {
          bodyCalls++;
          return originalBodyData.call(this);
        };
        parts.forEach((part, index) => {
          part.close = () => {
            closes[index]++;
            close[index]();
          };
        });
        const start = (onAdmitted) =>
          transport.request(
            'channel',
            'request',
            request,
            1000,
            controller.signal,
            undefined,
            () => {
              admitted++;
              onAdmitted?.();
            }
          );
        try {
          const pending =
            publicResult === 'submit' ? createZLinkRequest(start) : mapZLinkRequestReply(start());
          const caller = observe(pending).then((result) => {
            settlements++;
            return result;
          });
          if (terminalMode === 'admission-failure') admission.reject(reason);
          else admission.resolve();
          if (!['submit-failure', 'admission-failure'].includes(terminalMode)) {
            if (publicResult === 'submit') await pending.admitted;
            else await barrier();
            assert.equal(admitted, 1);
          }
          if (terminalMode === 'cancel') controller.abort(reason);
          if (terminalMode === 'deadline')
            runtime.registry.expire(performance.now() + 2000, Infinity);
          if (terminalMode === 'shutdown') runtime.close();
          if (terminalMode === 'shutdown-custom') runtime.close(reason);
          if (terminal === 'physical') nativeReply.reject(reason);
          if (['normal', 'codec', 'malformed', 'after-abort'].includes(terminalMode))
            nativeReply.resolve(parts);
          const result = await caller;
          if (terminal === 'after-abort') controller.abort(reason);
          if (terminal === 'normal' || terminal === 'after-abort') {
            assert.ifError(result.error);
            assert.deepEqual(result.value, { answer: 42 });
          } else if (['cancel', 'shutdown-custom', 'codec'].includes(terminalMode))
            assert.equal(result.error, reason);
          else if (terminal === 'malformed') assert.ok(result.error instanceof SyntaxError);
          else if (terminalMode === 'deadline')
            assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
          else if (terminalMode === 'shutdown')
            assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
          else {
            assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
            assert.equal(result.error.cause, reason);
          }
          if (
            !['normal', 'codec', 'malformed', 'after-abort', 'physical', 'submit-failure'].includes(
              terminalMode
            )
          )
            nativeReply.resolve(parts);
          await barrier();
          if (!['physical', 'submit-failure'].includes(terminalMode)) {
            assert.equal(codecs, 1);
            assert.equal(
              bodyCalls,
              terminal === 'codec' ||
                terminal === 'malformed' ||
                /-(codec|malformed)$/.test(terminal)
                ? 0
                : 1
            );
            assert.deepEqual(closes, [1, 1]);
          }
          assert.equal(runtime.registry.size, 0);
          assert.equal(await caller, result);
          assert.equal(settlements, 1);
          if (['submit-failure', 'admission-failure'].includes(terminalMode))
            assert.equal(admitted, 0);
        } finally {
          runtime.close();
          request.close();
          parts[0].data = originalData;
          parts[1].data = originalBodyData;
          parts.forEach((part, index) => {
            part.close = originalClose[index];
            if (closes[index] === 0) close[index]();
          });
        }
      });
    }
  }
}

for (const mapper of ['return', 'throw']) {
  test(`synchronous source submit failure maps once/${mapper}`, async () => {
    const runtime = new RequestRuntime();
    const entry = runtime.begin(1000);
    const original = new Error('submit');
    const mapped = new Error('mapped submit');
    let calls = 0;
    try {
      const result = await observe(
        runtime.run(
          entry,
          () => {
            throw original;
          },
          (value) => value,
          (error) => {
            calls++;
            assert.equal(error, original);
            if (mapper === 'throw') throw mapped;
            return mapped;
          }
        )
      );
      assert.equal(result.error, mapped);
      assert.equal(calls, 1);
      assert.equal(runtime.registry.size, 0);
    } finally {
      runtime.close();
    }
  });
}

for (const publicResult of ['create', 'map']) {
  for (const backpressured of [false, true]) {
    test(`#2911 Dealer codec abort keeps reply winner/${publicResult}/${backpressured ? 'Backpressured' : 'Ok'}`, async () => {
      const {
        ZLinkDealerChannelClientTransport
      } = require('../../packages/framework/dist/runtime/backend/node/node-dealer-channel-client-transport');
      const {
        createZLinkRequest,
        mapZLinkRequestReply
      } = require('../../packages/framework/dist/runtime/messaging/request-result');
      const runtime = new RequestRuntime();
      const controller = new AbortController();
      const physical = deferred();
      const admission = deferred();
      const reason = new Error('abort inside binding Message.data');
      const request = zlink.Message.from('{}');
      const parts = encodeChannelReplyParts(
        {
          channelName: 'channel',
          messageName: 'reply',
          correlationId: '0123456789abcdef0123456789abcdef'
        },
        { answer: 42 }
      ).map((value) => zlink.Message.from(value));
      const data = parts[1].data;
      const close = parts.map((part) => part.close);
      const closes = parts.map(() => 0);
      let codecs = 0,
        entry,
        pendingDuringCodec,
        sizeDuringCodec,
        admitted = 0,
        settlements = 0;
      parts[1].data = function () {
        codecs++;
        pendingDuringCodec = entry.isPending();
        sizeDuringCodec = runtime.registry.size;
        controller.abort(reason);
        return data.call(this);
      };
      parts.forEach((part, index) => {
        part.close = function () {
          closes[index]++;
          close[index].call(this);
        };
      });
      const builder = {
        message() {
          return this;
        },
        timeout() {
          return this;
        },
        submit() {
          return {
            result: backpressured ? zlink.SubmitResult.Backpressured : zlink.SubmitResult.Ok,
            admitted: admission.promise,
            reply: physical.promise
          };
        }
      };
      const transport = new ZLinkDealerChannelClientTransport(
        {
          request() {
            return builder;
          }
        },
        undefined,
        runtime
      );
      let carrier;
      const start = (onAdmitted) => {
        carrier = transport.request(
          'channel',
          'request',
          request,
          1000,
          controller.signal,
          undefined,
          () => {
            admitted++;
            onAdmitted?.();
          }
        );
        entry = carrier.operation;
        return carrier;
      };
      try {
        const pending =
          publicResult === 'create' ? createZLinkRequest(start) : mapZLinkRequestReply(start());
        const caller = observe(pending).then((result) => {
          settlements++;
          return result;
        });
        assert.equal(carrier.pending, entry.promise);
        assert.equal(pending, entry.promise);
        admission.resolve();
        physical.resolve(parts);
        const result = await caller;
        await barrier();
        assert.ifError(result.error);
        assert.deepEqual(result.value, { answer: 42 });
        assert.equal(pendingDuringCodec, false);
        assert.equal(sizeDuringCodec, 0);
        assert.equal(codecs, 1);
        assert.deepEqual(closes, [1, 1]);
        assert.equal(admitted, 1);
        assert.equal(settlements, 1);
        assert.equal(runtime.registry.size, 0);
      } finally {
        runtime.close();
        request.close();
        parts[1].data = data;
        parts.forEach((part, index) => {
          part.close = close[index];
          if (closes[index] === 0) close[index].call(part);
        });
      }
    });
  }
}

test('#2911 observe codec throw rejects directly without failure fallback', async () => {
  const runtime = new RequestRuntime();
  const codecError = new Error('observe codec');
  let entry,
    receiver,
    failed = 0,
    codecs = 0,
    pendingDuringCodec;
  try {
    const caller = observe(
      runtime.observe(
        1000,
        (value, physical) => {
          entry = value;
          receiver = physical;
        },
        () => {
          codecs++;
          pendingDuringCodec = entry.isPending();
          throw codecError;
        },
        () => {
          failed++;
          return 'incorrect fallback';
        }
      )
    );
    receiver.operationCompleted(entry.id, 'raw', undefined);
    const result = await caller;
    assert.equal(result.error, codecError);
    assert.equal(failed, 0);
    assert.equal(codecs, 1);
    assert.equal(pendingDuringCodec, false);
    assert.equal(runtime.registry.size, 0);
  } finally {
    runtime.close();
  }
});

for (const terminal of [
  'normal',
  'codec-abort',
  'cancel',
  'deadline',
  'shutdown',
  'shutdown-custom'
]) {
  for (const throwing of [false, true]) {
    test(`#2911 observe retained codec/${terminal}/throw=${throwing}`, async () => {
      const runtime = new RequestRuntime();
      const controller = new AbortController();
      const reason = new Error(terminal);
      const codecError = new Error('late codec');
      let entry,
        receiver,
        codecs = 0,
        failed = 0,
        pendingDuringCodec,
        failure;
      try {
        const caller = observe(
          runtime.observe(
            1000,
            (value, physical) => {
              entry = value;
              receiver = physical;
              entry.setSignal(controller.signal);
            },
            (value) => {
              codecs++;
              pendingDuringCodec = entry.isPending();
              if (terminal === 'codec-abort') controller.abort(reason);
              if (throwing) throw codecError;
              return value + 1;
            },
            (error) => {
              failed++;
              failure = error;
              throw error;
            }
          )
        );
        if (terminal === 'cancel') controller.abort(reason);
        if (terminal === 'deadline') runtime.registry.expire(performance.now() + 2000, Infinity);
        if (terminal === 'shutdown') runtime.close();
        if (terminal === 'shutdown-custom') runtime.close(reason);
        const late = !['normal', 'codec-abort'].includes(terminal);
        const before = late ? await caller : undefined;
        receiver.operationCompleted(entry.id, 2, undefined);
        const result = await caller;
        if (late) {
          assert.equal(result, before);
          assert.equal(failed, 1);
          assert.equal(result.error, failure);
          if (terminal === 'cancel' || terminal === 'shutdown-custom')
            assert.equal(result.error, reason);
          if (terminal === 'deadline')
            assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
          if (terminal === 'shutdown')
            assert.equal(result.error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
        } else {
          assert.equal(failed, 0);
          if (throwing) assert.equal(result.error, codecError);
          else assert.deepEqual(result, { value: 3 });
        }
        assert.equal(pendingDuringCodec, false);
        assert.equal(codecs, 1);
        assert.equal(runtime.registry.size, 0);
      } finally {
        runtime.close();
      }
    });
  }
}

for (const throwing of [false, true]) {
  for (const source of ['receiver', 'submit']) {
    test(`#2911 observe source failure/${source}/throw=${throwing}`, async () => {
      const runtime = new RequestRuntime();
      const reason = new Error('source');
      const mapped = new Error('onFailed');
      let receiver,
        entry,
        calls = 0,
        failure,
        codecs = 0;
      try {
        const caller = observe(
          runtime.observe(
            1000,
            (value, physical) => {
              entry = value;
              receiver = physical;
              if (source === 'submit') throw reason;
            },
            (value) => {
              codecs++;
              return value;
            },
            (error) => {
              calls++;
              failure = error;
              if (throwing) throw mapped;
              return 'fallback';
            }
          )
        );
        if (source === 'receiver') receiver.operationFailed(entry.id, reason, undefined);
        const result = await caller;
        assert.equal(failure, reason);
        assert.equal(calls, 1);
        assert.equal(codecs, 0);
        if (throwing) assert.equal(result.error, mapped);
        else assert.deepEqual(result, { value: 'fallback' });
        assert.equal(runtime.registry.size, 0);
      } finally {
        runtime.close();
      }
    });
  }
}

for (const terminal of ['cancel', 'shutdown', 'build-throw']) {
  test(`#2911 Dealer terminal during builder blocks submit/${terminal}`, async () => {
    const {
      ZLinkDealerChannelClientTransport
    } = require('../../packages/framework/dist/runtime/backend/node/node-dealer-channel-client-transport');
    const {
      createZLinkRequest
    } = require('../../packages/framework/dist/runtime/messaging/request-result');
    const runtime = new RequestRuntime();
    const controller = new AbortController();
    const reason = new Error(terminal);
    const request = zlink.Message.from('{}');
    let submissions = 0;
    const builder = {
      message() {
        if (terminal === 'build-throw') throw reason;
        if (terminal === 'cancel') controller.abort(reason);
        if (terminal === 'shutdown') runtime.close(reason);
        return this;
      },
      timeout() {
        return this;
      },
      submit() {
        submissions++;
        throw new Error('unexpected submit');
      }
    };
    const transport = new ZLinkDealerChannelClientTransport(
      {
        request() {
          return builder;
        }
      },
      undefined,
      runtime
    );
    try {
      const pending = createZLinkRequest((admitted) =>
        transport.request(
          'channel',
          'request',
          request,
          1000,
          controller.signal,
          undefined,
          admitted
        )
      );
      const outcomes = await Promise.allSettled([pending, pending.admitted]);
      assert.deepEqual(
        outcomes.map((value) => value.status),
        ['rejected', 'rejected']
      );
      assert.equal(outcomes[0].reason, reason);
      assert.equal(outcomes[1].reason, reason);
      assert.equal(submissions, 0);
      assert.equal(runtime.registry.size, 0);
    } finally {
      runtime.close();
      request.close();
    }
  });
}

for (const boundary of ['source', 'codec']) {
  for (const throwing of [false, true]) {
    test(`#2911 actual Dealer failure mapper/${boundary}/throw=${throwing}`, async (t) => {
      const {
        ZLinkDealerChannelClientTransport
      } = require('../../packages/framework/dist/runtime/backend/node/node-dealer-channel-client-transport');
      const {
        createZLinkRequest
      } = require('../../packages/framework/dist/runtime/messaging/request-result');
      const multipart = require('../../packages/framework/dist/runtime/channels/channel-multipart');
      const runtime = new RequestRuntime();
      const physical = deferred(),
        admission = deferred();
      const reason = new Error(boundary),
        mapped = new Error('source mapped');
      const request = zlink.Message.from('{}');
      const parts = encodeChannelReplyParts(
        {
          channelName: 'channel',
          messageName: 'reply',
          correlationId: '0123456789abcdef0123456789abcdef'
        },
        { answer: 42 }
      ).map((value) => zlink.Message.from(value));
      const data = parts[1].data;
      const close = parts.map((part) => part.close),
        closes = [0, 0];
      let calls = 0,
        sourceError,
        sourceLabel,
        codecs = 0;
      t.mock.method(multipart, 'mapRequestFailure', (error, label) => {
        calls++;
        sourceError = error;
        sourceLabel = label;
        if (throwing) throw mapped;
        return mapped;
      });
      parts[1].data = function () {
        codecs++;
        throw reason;
      };
      parts.forEach((part, index) => {
        part.close = function () {
          closes[index]++;
          close[index].call(this);
        };
      });
      const builder = {
        message() {
          return this;
        },
        timeout() {
          return this;
        },
        submit() {
          return {
            result: zlink.SubmitResult.Backpressured,
            admitted: admission.promise,
            reply: physical.promise
          };
        }
      };
      const transport = new ZLinkDealerChannelClientTransport(
        {
          request() {
            return builder;
          }
        },
        undefined,
        runtime
      );
      try {
        const pending = createZLinkRequest((admitted) =>
          transport.request('channel', 'request', request, 1000, undefined, undefined, admitted)
        );
        const outcomes = Promise.allSettled([pending, pending.admitted]);
        if (boundary === 'source') physical.reject(reason);
        else physical.resolve(parts);
        const results = await outcomes;
        admission.resolve();
        await barrier();
        assert.deepEqual(
          results.map((result) => result.status),
          ['rejected', 'rejected']
        );
        assert.equal(results[0].reason, boundary === 'source' ? mapped : reason);
        assert.equal(results[1].reason, results[0].reason);
        assert.equal(calls, boundary === 'source' ? 1 : 0);
        assert.equal(codecs, boundary === 'codec' ? 1 : 0);
        assert.deepEqual(closes, boundary === 'codec' ? [1, 1] : [0, 0]);
        if (boundary === 'source') {
          assert.equal(sourceError, reason);
          assert.equal(sourceLabel, 'channel request');
        }
        assert.equal(runtime.registry.size, 0);
      } finally {
        t.mock.restoreAll();
        runtime.close();
        request.close();
        parts[1].data = data;
        parts.forEach((part, index) => {
          part.close = close[index];
          if (closes[index] === 0) close[index].call(part);
        });
      }
    });
  }
}

for (const throwing of [false, true]) {
  test(`#2911 production ClientServer pending and trace settle once/upstream-throw=${throwing}`, async () => {
    const runtime = new RequestRuntime();
    const entry = runtime.begin(1000);
    const reason = new Error('upstream mapper');
    const physical = deferred();
    const submitted = deferred();
    const events = [];
    let mapped = 0;
    entry.mapCaller(
      (value) => {
        mapped++;
        if (throwing) throw reason;
        return value;
      },
      (error) => error
    );
    const socket = wrapSocket({
      request() {
        return {
          message() {
            return this;
          },
          timeout() {
            return this;
          },
          submit() {
            submitted.resolve();
            return {
              result: zlink.SubmitResult.Ok,
              admitted: Promise.resolve(),
              reply: physical.promise
            };
          }
        };
      },
      close() {}
    });
    const outbound = new ZLinkChannelOutboundOperations(
      {
        async awaitClientDealerForOutbound() {
          return socket;
        },
        selectedClientServerRid() {
          return 'server';
        }
      },
      undefined,
      {
        flowCreationEnabled() {
          return false;
        },
        beginOutbound(outcome, result) {
          return {
            trace() {
              events.push({ outcome, result });
            }
          };
        }
      },
      runtime
    );
    const parts = encodeChannelReplyParts(
      {
        channelName: 'channel',
        messageName: 'reply',
        correlationId: '0123456789abcdef0123456789abcdef'
      },
      { answer: 42 }
    ).map((value) => zlink.Message.from(value));
    const close = parts.map((part) => part.close),
      closes = [0, 0];
    parts.forEach((part, index) => {
      part.close = function () {
        closes[index]++;
        close[index].call(this);
      };
    });
    try {
      const pending = outbound.request(
        'channel',
        'request',
        {},
        1000,
        undefined,
        undefined,
        undefined,
        entry
      );
      const caller = observe(pending);
      assert.equal(pending, entry.promise);
      assert.equal(outbound.pendingRequestCount('channel'), 1);
      await submitted.promise;
      physical.resolve(parts);
      const result = await caller;
      await barrier();
      if (throwing) assert.equal(result.error, reason);
      else assert.deepEqual(result.value, { answer: 42 });
      assert.equal(mapped, 1);
      assert.equal(outbound.pendingRequestCount('channel'), 0);
      assert.equal(runtime.registry.size, 0);
      assert.deepEqual(closes, [1, 1]);
      const terminals = events.filter(
        (event) => event.outcome === ZLinkRuntimeMessageFlowOutcome.ReplyReceived
      );
      assert.equal(terminals.length, 1);
      assert.equal(terminals[0].result, throwing ? 'failed' : 'succeeded');
    } finally {
      runtime.close();
      parts.forEach((part, index) => {
        part.close = close[index];
        if (closes[index] === 0) close[index].call(part);
      });
      await socket.dispose();
    }
  });
}
// Exercise the actual manager/backend/public mapper chain in a plain Node child so
// an unhandled rejection is measured without node:test consuming the event first.
function fallbackCompletionContractProbe() {
  const path = require('node:path');
  const { createRequire } = require('node:module');
  const assert = require('node:assert/strict');
  const { setImmediate: barrier } = require('node:timers/promises');
  const root = process.cwd();
  const req = createRequire(root + '/package.json');
  const dist = root + '/packages/framework/dist/';
  const { RequestRuntime } = require(dist + 'runtime/foundation/request-runtime');
  const { ZLinkRuntimeRouteTransport } = require(dist + 'runtime/channels/channel-transports');
  const { ZLinkChannelRuntimeManager } = require(dist + 'runtime/channels/channel-runtime-manager');
  const { createZLinkRequest, mapZLinkRequestReply } = require(
    dist + 'runtime/messaging/request-result'
  );
  const { wrapSocket } = require(dist + 'runtime/backend/node/node-socket-backend-adapter');
  const fw = require(dist + 'internal');
  const zlink = req('@zlink-systems/zlink');
  const observe = (p) =>
    p.then(
      (value) => ({ value }),
      (error) => ({ error })
    );
  function deferred() {
    let resolve, reject;
    const promise = new Promise((a, b) => {
      resolve = a;
      reject = b;
    });
    return { promise, resolve, reject };
  }
  const target = {
    routerChannelId: 'route',
    targetNodeRid: 'peer',
    spotId: 'spot'
  };
  function route(manager, runtime) {
    return new ZLinkRuntimeRouteTransport(
      () => manager,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      () => false,
      undefined,
      runtime
    );
  }
  function start(t, kind, onAdmitted, signal) {
    return kind === 'node'
      ? t.request('route', 'peer', 'Probe', {}, 1000, signal, undefined, onAdmitted)
      : t.requestToSpot(target, {}, { packetName: 'Probe', timeoutMs: 1000, signal, onAdmitted });
  }
  const rows = [];
  let failures = 0;
  function log(row) {
    rows.push(row);
    console.log(JSON.stringify(row));
  }
  async function syncProbe(kind, mode) {
    const runtime = new RequestRuntime();
    const cause = new Error('manager-sync-' + kind);
    let calls = 0;
    const manager =
      mode === 'missing'
        ? undefined
        : {
            routeRequest() {
              calls++;
              throw cause;
            },
            routeRequestToSpot() {
              calls++;
              throw cause;
            }
          };
    const t = route(manager, runtime);
    let thrown;
    try {
      const request = createZLinkRequest((admitted) => {
        try {
          return start(t, kind, admitted);
        } catch (e) {
          thrown = e;
          throw e;
        }
      });
      const [reply, admitted] = await Promise.all([observe(request), observe(request.admitted)]);
      const row = {
        case: 'sync-' + kind + '-' + mode,
        sizeAtCallerTerminal: runtime.registry.size,
        expectedSize: 0,
        submit: 0,
        managerCalls: calls,
        reply: reply.error?.message,
        admitted: admitted.error?.message,
        sameError: reply.error === admitted.error,
        originalIdentity: reply.error === thrown,
        providedCause: mode === 'missing' ? null : reply.error === cause
      };
      log(row);
      assert.equal(row.sizeAtCallerTerminal, 0);
      assert(row.sameError);
      if (thrown) assert(row.originalIdentity);
      assert.equal(calls, mode === 'missing' ? 0 : 1);
    } finally {
      // Public registry handle is used only after the observation to drain the orphan on close.
      const cleanup = runtime.registry.size
        ? observe(runtime.registry.handle(1n).promise)
        : Promise.resolve();
      runtime.close();
      await cleanup;
      await barrier();
      log({
        case: 'sync-cleanup-' + kind + '-' + mode,
        size: runtime.registry.size
      });
    }
  }
  async function asyncProbe(kind, consumer, terminal) {
    const runtime = new RequestRuntime(),
      controller = new AbortController();
    const cause = new Error('source-' + kind + '-' + terminal);
    const physical = deferred(),
      admission = deferred();
    let builders = 0,
      submits = 0;
    const payloads = [];
    const builder = () => {
      builders++;
      if (terminal === 'builder-throw') throw cause;
      return {
        message(m) {
          payloads.push(m);
          return this;
        },
        timeout() {
          return this;
        },
        submit() {
          submits++;
          return {
            result: zlink.SubmitResult.Ok,
            admitted: admission.promise,
            reply: physical.promise
          };
        }
      };
    };
    const native = {
      close() {},
      options: {},
      setChannelName() {},
      setRoutingId() {},
      setReceiveFlowState() {},
      bind() {},
      connect() {},
      request: builder,
      requestToSpot: builder,
      reply() {}
    };
    const socket = { ...wrapSocket(native), setReceiveFlowState() {} };
    const registration = fw.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'route' }]
    });
    const manager = new ZLinkChannelRuntimeManager(
      registration,
      { createRouterSocket: () => socket },
      {},
      undefined,
      { requests: runtime }
    );
    const t = route(manager, runtime);
    let carrier, entry, publicRequest, publicOutcome, admittedOutcome;
    const events = [];
    const listener = (reason, promise) => events.push({ reason, promise });
    process.on('unhandledRejection', listener);
    const beforeListeners = process.listenerCount('unhandledRejection') - 1;
    try {
      if (consumer === 'direct') {
        entry = runtime.begin(1000, controller.signal);
        const pending =
          kind === 'node'
            ? manager.routeRequest(
                'route',
                'peer',
                'Probe',
                {},
                1000,
                controller.signal,
                undefined,
                undefined,
                entry
              )
            : manager.routeRequestToSpot(
                target,
                'Probe',
                {},
                1000,
                controller.signal,
                undefined,
                undefined,
                entry
              );
        carrier = { pending, operation: entry };
        publicOutcome = observe(pending);
      } else if (consumer === 'create') {
        publicRequest = createZLinkRequest((admitted) => {
          carrier = start(t, kind, admitted, controller.signal);
          return carrier;
        });
        entry = carrier.operation;
        publicOutcome = observe(publicRequest);
        admittedOutcome = observe(publicRequest.admitted);
      } else {
        carrier = start(t, kind, undefined, controller.signal);
        entry = carrier.operation;
        publicRequest = mapZLinkRequestReply(carrier);
        publicOutcome = observe(publicRequest);
      }
      if (terminal === 'cancel') controller.abort(cause);
      if (terminal === 'physical-fail') physical.reject(cause);
      const outcome = await publicOutcome;
      const admitted = admittedOutcome ? await admittedOutcome : undefined;
      await barrier();
      await barrier();
      const row = {
        case: `async-${kind}-${consumer}-${terminal}`,
        pendingEqualsEntry: carrier.pending === entry.promise,
        publicEqualsEntry: publicRequest ? publicRequest === entry.promise : null,
        replyError: outcome.error?.stack,
        replyIsCause: outcome.error === cause,
        admissionStatus: admitted
          ? Object.hasOwn(admitted, 'error')
            ? 'rejected'
            : 'fulfilled'
          : null,
        admittedIsCause: admitted ? admitted.error === cause : null,
        size: runtime.registry.size,
        builders,
        submits,
        unhandled: events.map((e) => ({
          message: e.reason?.message,
          reasonIsCause: e.reason === cause,
          promiseIsPending: e.promise === carrier.pending,
          promiseIsEntry: e.promise === entry.promise
        }))
      };
      log(row);
      assert(row.replyIsCause);
      assert.equal(row.size, 0);
      assert.equal(builders, 1);
      assert.equal(submits, terminal === 'builder-throw' ? 0 : 1);
      assert.equal(events.length, 0);
      if (admitted)
        assert.equal(row.admissionStatus, terminal === 'builder-throw' ? 'rejected' : 'fulfilled');
      if (events.length) {
        assert(events[0].promise === carrier.pending);
        assert(events[0].reason === cause);
      }
    } finally {
      // Settle the physical fixture only after recording public/unhandled observations.
      admission.reject(cause);
      physical.reject(cause);
      // Unused deferreds in builder-throw were never supplied to the product.
      await Promise.all([observe(admission.promise), observe(physical.promise)]);
      runtime.close();
      await socket.dispose();
      await barrier();
      await barrier();
      for (const m of payloads) if (typeof m.close === 'function') m.close();
      process.removeListener('unhandledRejection', listener);
      log({
        case: `async-cleanup-${kind}-${consumer}-${terminal}`,
        size: runtime.registry.size,
        listenersRestored: process.listenerCount('unhandledRejection') === beforeListeners
      });
    }
  }
  (async () => {
    for (const kind of ['node', 'spot'])
      for (const mode of ['missing', 'throw']) {
        try {
          await syncProbe(kind, mode);
        } catch (e) {
          failures++;
          console.log('ASSERTION_FAILURE', e.stack);
        }
      }
    for (const kind of ['node', 'spot'])
      for (const terminal of ['builder-throw', 'cancel', 'physical-fail'])
        for (const consumer of ['direct', 'create', 'map']) {
          try {
            await asyncProbe(kind, consumer, terminal);
          } catch (e) {
            failures++;
            console.log('ASSERTION_FAILURE', e.stack);
          }
        }
    console.log(
      JSON.stringify({
        summary: true,
        cases: rows.filter((r) => !r.case.includes('cleanup')).length,
        assertionFailures: failures
      })
    );
    process.exitCode = failures ? 1 : 0;
  })().catch((e) => {
    console.error('HARNESS_FAILURE', e.stack);
    process.exitCode = 2;
  });
}

test('fallback manager start and asynchronous physical failures finish one entry without unhandled results', () => {
  const { spawnSync } = require('node:child_process');
  const path = require('node:path');
  const result = spawnSync(
    process.execPath,
    ['-e', `(${fallbackCompletionContractProbe.toString()})()`],
    {
      cwd: path.resolve(__dirname, '../..'),
      encoding: 'utf8',
      timeout: 30000
    }
  );
  assert.equal(result.error, undefined);
  assert.equal(result.signal, null);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /"cases":22,"assertionFailures":0/);
});
// Shared runtime ownership survives both actual physical success and local application work.
for (const kind of ['node', 'spot', 'local']) {
  for (const terminal of ['normal', 'failure', 'pre-abort']) {
    test(`fallback ${kind}/${terminal} retains the supplied entry and source outcome`, async () => {
      const {
        ZLinkChannelRuntimeManager
      } = require('../../packages/framework/dist/runtime/channels/channel-runtime-manager');
      const {
        ZLinkRuntimeRouteTransport
      } = require('../../packages/framework/dist/runtime/channels/channel-transports');
      const {
        createZLinkRequest
      } = require('../../packages/framework/dist/runtime/messaging/request-result');
      const runtime = new RequestRuntime();
      const controller = new AbortController();
      const cause = new Error(terminal);
      if (terminal === 'pre-abort') controller.abort(cause);
      const physical = deferred();
      const application = deferred();
      let builders = 0,
        submits = 0,
        localCalls = 0,
        seenEntry;
      const socket = {
        ...wrapSocket({
          close() {},
          options: {},
          setChannelName() {},
          setRoutingId() {},
          setReceiveFlowState() {},
          bind() {},
          connect() {},
          reply() {},
          request() {
            builders++;
            return builder;
          },
          requestToSpot() {
            builders++;
            return builder;
          }
        }),
        setReceiveFlowState() {}
      };
      const builder = {
        message() {
          return this;
        },
        timeout() {
          return this;
        },
        submit() {
          submits++;
          return {
            result: zlink.SubmitResult.Ok,
            admitted: Promise.resolve(),
            reply: physical.promise
          };
        }
      };
      const registration = framework.createFrameworkRegistration({
        routeChannels: [{ routerChannelId: 'route' }]
      });
      const manager = new ZLinkChannelRuntimeManager(
        registration,
        { createRouterSocket: () => socket },
        {},
        undefined,
        {
          requests: runtime,
          localSpotRouteDispatcher: {
            request(spotId, packetName, value, context) {
              localCalls++;
              seenEntry = context.requestOperation;
              assert.equal(spotId, 'spot');
              assert.equal(packetName, 'Probe');
              assert.equal(context.channelName, 'route');
              context.onAdmitted?.();
              return application.promise;
            }
          }
        }
      );
      if (kind === 'local') manager.setSpotNodes(new Map([['route', { routingId: 'peer' }]]));
      const transport = new ZLinkRuntimeRouteTransport(
        () => manager,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        () => false,
        undefined,
        runtime
      );
      const target = {
        routerChannelId: 'route',
        targetNodeRid: 'peer',
        spotId: 'spot'
      };
      const entry = terminal === 'pre-abort' ? undefined : runtime.begin(1000, controller.signal);
      let carrier;
      const request = createZLinkRequest((onAdmitted) => {
        carrier =
          kind === 'node'
            ? transport.request(
                'route',
                'peer',
                'Probe',
                {},
                1000,
                controller.signal,
                undefined,
                onAdmitted,
                entry
              )
            : transport.requestToSpot(
                target,
                {},
                {
                  packetName: 'Probe',
                  timeoutMs: 1000,
                  signal: controller.signal,
                  onAdmitted,
                  requestOperation: entry
                }
              );
        return carrier;
      });
      const outcome = observe(request);
      const admission = observe(request.admitted);
      try {
        if (terminal !== 'pre-abort') {
          assert.equal(carrier.operation, entry);
          assert.equal(carrier.pending, entry.promise);
          assert.equal(request, entry.promise);
          assert.equal(runtime.registry.size, 1);
          if (kind === 'local') assert.equal(seenEntry, entry);
          if (terminal === 'failure') {
            if (kind === 'local') application.reject(cause);
            else physical.reject(cause);
          } else if (kind === 'local') application.resolve({ answer: 42 });
          else
            physical.resolve(
              encodeChannelReplyParts(
                {
                  channelName: 'route',
                  messageName: 'Reply',
                  correlationId: '0123456789abcdef0123456789abcdef'
                },
                { answer: 42 }
              ).map((part) =>
                require('../../packages/framework/dist/runtime/backend/runtime-message').ZLinkBufferMessage.from(
                  part
                )
              )
            );
        }
        const result = await outcome;
        const admitted = await admission;
        if (terminal === 'normal') assert.deepEqual(result.value, { answer: 42 });
        else assert.equal(result.error, cause);
        if (terminal === 'pre-abort') assert.equal(admitted.error, cause);
        else assert.deepEqual(admitted, { value: undefined });
        assert.equal(builders, kind === 'local' || terminal === 'pre-abort' ? 0 : 1);
        assert.equal(submits, kind === 'local' || terminal === 'pre-abort' ? 0 : 1);
        assert.equal(localCalls, kind === 'local' && terminal !== 'pre-abort' ? 1 : 0);
        assert.equal(runtime.registry.size, 0);
      } finally {
        physical.reject(cause);
        application.reject(cause);
        await Promise.all([observe(physical.promise), observe(application.promise)]);
        runtime.close();
        await socket.dispose();
      }
    });
  }
}

for (const boundary of ['node', 'spot', 'router', 'bridge']) {
  test(`direct ${boundary} pre-entry error retains rejected Promise surface`, async () => {
    const {
      ZLinkChannelRuntimeManager
    } = require('../../packages/framework/dist/runtime/channels/channel-runtime-manager');
    const {
      ZLinkRouterSocketSpotRouteTransport
    } = require('../../packages/framework/dist/runtime/channels/router-socket-spot-route-transport');
    const {
      ZLinkSpotRouteBridgeTransport
    } = require('../../packages/framework/dist/runtime/channels/spot-route-bridge-transport');
    const runtime = new RequestRuntime();
    runtime.registry.close();
    const target = {
      routerChannelId: 'route',
      targetNodeRid: 'peer',
      spotId: 'spot'
    };
    const manager = new ZLinkChannelRuntimeManager(
      framework.createFrameworkRegistration(),
      {},
      {},
      undefined,
      { requests: runtime }
    );
    const router = new ZLinkRouterSocketSpotRouteTransport({}, runtime);
    const bridge = new ZLinkSpotRouteBridgeTransport(new Map(), 50, runtime);
    let pending;
    assert.doesNotThrow(() => {
      if (boundary === 'node') pending = manager.routeRequest('route', 'peer', 'Probe', {}, 50);
      if (boundary === 'spot') pending = manager.routeRequestToSpot(target, 'Probe', {}, 50);
      if (boundary === 'router') pending = router.request(target, [], undefined, 0, 50);
      if (boundary === 'bridge') pending = bridge.request(target, [], undefined, 50);
    });
    assert.ok(pending instanceof Promise);
    await assert.rejects(pending, /closed/);
    assert.equal(runtime.registry.size, 0);
  });
}

for (const boundary of ['router', 'bound-router', 'named', 'bridge', 'local']) {
  for (const terminal of ['normal', 'failure']) {
    test(`direct Spot ${boundary}/${terminal} preserves branch timeout defaults`, async (t) => {
      const {
        ZLinkSpotRouteDispatchStrategy
      } = require('../../packages/framework/dist/runtime/channels/spot-route-dispatch-strategy');
      t.mock.method(performance, 'now', () => 0);
      const runtime = new RequestRuntime();
      const work = deferred();
      const cause = new Error('direct source failure');
      let entry,
        receiver,
        sourceCalls = 0;
      const target = {
        routerChannelId: 'route',
        targetNodeRid: 'peer',
        spotId: 'spot'
      };
      const registration = framework.createFrameworkRegistration({
        requestTimeoutMs: 50,
        routeChannels: [
          {
            routerChannelId: 'route',
            ...(boundary === 'bound-router' ? { bind: 'inproc://direct-default' } : {})
          }
        ],
        ...(boundary === 'named'
          ? { spotNodes: { route: { router: { bind: 'inproc://named-default' } } } }
          : {})
      });
      const routeSocket = {
        requestToSpot(_node, _spot, _parts, timeout, _admitted, operation, observer) {
          sourceCalls++;
          entry = operation;
          receiver = observer;
          assert.equal(timeout, boundary === 'named' ? 50 : undefined);
        }
      };
      const bridge = {
        request() {
          return {
            message() {
              return this;
            },
            timeout(value) {
              assert.equal(value, 50);
              return this;
            },
            submit(_admitted, operation, observer) {
              sourceCalls++;
              entry = operation;
              receiver = observer;
              return entry.promise;
            }
          };
        }
      };
      const strategy = new ZLinkSpotRouteDispatchStrategy({
        requests: runtime,
        registration,
        codecs: { serializers: new Map() },
        spotRouteBridges: new Map(boundary === 'bridge' ? [['route', bridge]] : []),
        flowCreationEnabled: () => false,
        sockets: {
          routeRouter() {
            return routeSocket;
          }
        },
        localSpotRouteDispatcher: {
          request(_spot, _packet, _value, context) {
            sourceCalls++;
            entry = context.requestOperation;
            return work.promise;
          }
        }
      });
      if (boundary === 'named')
        strategy.setSpotNodes(
          new Map([
            [
              'route',
              {
                routingId: 'other',
                entrySpot() {
                  return routeSocket;
                }
              }
            ]
          ])
        );
      if (boundary === 'local') strategy.setSpotNodes(new Map([['route', { routingId: 'peer' }]]));
      const pending = strategy.routeRequestToSpot(target, 'Probe', {}, undefined);
      const outcome = observe(pending);
      try {
        assert.equal(sourceCalls, 1);
        assert.equal(runtime.registry.size, 1);
        assert.equal(
          entry.deadlineMs,
          boundary === 'router' || boundary === 'bound-router' ? undefined : 50
        );
        assert.equal(pending, entry.promise);
        if (boundary === 'local') {
          if (terminal === 'normal') work.resolve({ answer: 42 });
          else work.reject(cause);
        } else if (terminal === 'normal')
          receiver.operationCompleted(
            entry.id,
            encodeChannelReplyParts(
              {
                channelName: 'route',
                messageName: 'Reply',
                correlationId: '0123456789abcdef0123456789abcdef'
              },
              { answer: 42 }
            ).map((part) =>
              require('../../packages/framework/dist/runtime/backend/runtime-message').ZLinkBufferMessage.from(
                part
              )
            ),
            undefined
          );
        else receiver.operationFailed(entry.id, cause, undefined);
        const result = await outcome;
        if (terminal === 'normal') assert.deepEqual(result.value, { answer: 42 });
        else assert.equal(result.error, cause);
        assert.equal(runtime.registry.size, 0);
      } finally {
        work.reject(cause);
        await observe(work.promise);
        runtime.close();
      }
    });
  }
}
for (const kind of ['node', 'spot']) {
  for (const failureAt of ['encode', 'pre-abort']) {
    test(`direct manager ${kind}/${failureAt} preserves source rejection without residual entry`, async () => {
      const {
        ZLinkChannelRuntimeManager
      } = require('../../packages/framework/dist/runtime/channels/channel-runtime-manager');
      const runtime = new RequestRuntime();
      const cause = new Error(failureAt);
      const controller = new AbortController();
      if (failureAt === 'pre-abort') controller.abort(cause);
      let builders = 0,
        serializations = 0;
      const socket = {
        ...wrapSocket({
          close() {},
          options: {},
          setChannelName() {},
          setRoutingId() {},
          setReceiveFlowState() {},
          bind() {},
          connect() {},
          request() {
            builders++;
            throw new Error('must encode or abort before builder');
          },
          requestToSpot() {
            builders++;
            throw new Error('must encode or abort before builder');
          }
        }),
        setReceiveFlowState() {}
      };
      const registration = framework.createFrameworkRegistration({
        routeChannels: [{ routerChannelId: 'route' }]
      });
      const manager = new ZLinkChannelRuntimeManager(
        registration,
        { createRouterSocket: () => socket },
        {},
        undefined,
        { requests: runtime }
      );
      const payload = {
        get value() {
          serializations++;
          throw cause;
        }
      };
      let pending;
      try {
        assert.doesNotThrow(() => {
          pending =
            kind === 'node'
              ? manager.routeRequest('route', 'peer', 'Probe', payload, 1000, controller.signal)
              : manager.routeRequestToSpot(
                  { routerChannelId: 'route', targetNodeRid: 'peer', spotId: 'spot' },
                  'Probe',
                  payload,
                  1000,
                  controller.signal
                );
        });
        assert.ok(pending instanceof Promise);
        const result = await observe(pending);
        if (kind === 'spot' && failureAt === 'pre-abort') {
          const { ZLinkAbortError } = require('../../packages/framework/dist/runtime/abort');
          assert.ok(result.error instanceof ZLinkAbortError);
          assert.equal(result.error.name, 'AbortError');
          assert.notEqual(result.error, cause);
        } else assert.equal(result.error, cause);
        assert.equal(runtime.registry.size, 0);
        assert.equal(builders, 0);
        assert.equal(serializations, failureAt === 'encode' ? 1 : 0);
      } finally {
        runtime.close();
        await socket.dispose();
      }
    });
  }
}

for (const shared of [false, true]) {
  for (const kind of ['userSpotCreate', 'userSpotClose', 'actorCreate']) {
    for (const terminal of ['cancel', 'deadline', 'shutdown', 'lifecycle']) {
      for (const codec of ['valid', 'malformed', 'throwing']) {
        test(`durable physical receiver/${shared ? 'shared' : 'standalone'}/${kind}/${terminal}/${codec}`, async (t) => {
          const {
            ServiceStatefulRuntime
          } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
          const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
          const {
            RequestResult
          } = require('../../packages/framework/dist/runtime/backend/runtime-values');
          let now = 1000;
          t.mock.method(performance, 'now', () => now);
          t.mock.timers.enable({ apis: ['setTimeout'] });
          const requests = shared ? new RequestRuntime() : undefined;
          const physical = deferred();
          let submitted,
            correlation,
            intentRemoved,
            codecCalls = 0,
            submissions = 0;
          const decode = wire.decodeStatefulReply;
          t.mock.method(wire, 'decodeStatefulReply', (...args) => {
            codecCalls++;
            if (codec === 'throwing') throw new Error('late codec failure');
            return decode(...args);
          });
          const runtime = new ServiceStatefulRuntime(
            {
              setServiceIngress() {},
              observePeerRoute: () => ({ ready: true }),
              observePeerRouteChanged: () => () => {},
              observePeerConnectionIntentRemoved(listener) {
                intentRemoved = listener;
                return () => {};
              },
              requestService(_rid, _parts, timeoutMs, _admitted, pending) {
                submissions++;
                submitted = pending;
                correlation = wire.decodeStatefulHeader(_parts[0]).correlation;
                assert.equal(timeoutMs, 50);
                assert.equal(pending.deadlineMs, 1050);
                return physical.promise;
              }
            },
            'source',
            7n,
            undefined,
            requests
          );
          try {
            const common = {
              sourceNodeRid: 'source',
              sourceNodeGeneration: 7n,
              deadlineUnixMs: BigInt(Date.now() + 50)
            };
            const reservation = {
              reservationId: 'reservation',
              expectedStoreVersion: 'version',
              objectGeneration: 3n,
              authorityOwnerGeneration: 5n,
              targetNodeRid: 'target',
              targetNodeGeneration: 7n,
              targetOwnerId: 'target-owner',
              targetOwnerLeaseGeneration: 7n,
              pendingCapacityDelta: 1
            };
            const caller = observe(
              kind === 'userSpotCreate'
                ? runtime.requestUserSpotCreate(
                    'target',
                    { ...common, spotId: 'spot', stableType: 'Room', reservation },
                    50
                  )
                : kind === 'actorCreate'
                  ? runtime.requestActorCreate(
                      'target',
                      {
                        ...common,
                        operation:
                          require('../../packages/framework/dist/runtime/locations/creation-operation-id').randomOperationId(),
                        actorId: 'actor',
                        stableType: 'Player',
                        reservation
                      },
                      50
                    )
                  : runtime.requestUserSpotClose(
                      'target',
                      {
                        ...common,
                        target: {
                          spotId: 'spot',
                          objectGeneration: 3n,
                          targetNodeRid: 'target',
                          targetNodeGeneration: 7n,
                          authorityOwnerGeneration: 5n,
                          expectedStoreVersion: 'version'
                        }
                      },
                      50
                    )
            );
            await barrier();
            assert.equal(submissions, 1);
            assert(submitted.isPending());
            if (shared) assert.equal(requests.registry.size, 1);
            const cause = new Error('early public terminal');
            if (terminal === 'cancel') runtime.cancelPendingOperations(cause);
            if (terminal === 'shutdown') {
              runtime.close();
              requests?.close();
            }
            if (terminal === 'lifecycle') intentRemoved('target');
            if (terminal === 'deadline') {
              now += 50;
              if (shared) t.mock.timers.tick(50);
              else assert.equal(runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
            }
            const early = await caller;
            assert(early.error);
            if (terminal === 'deadline')
              assert.equal(early.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
            assert.equal(codecCalls, 0);
            assert.equal(submitted.isPending(), false);
            if (shared) assert.equal(requests.registry.size, 0);
            physical.resolve(
              codec === 'malformed'
                ? [Buffer.from('malformed')]
                : [wire.encodeStatefulReply(correlation, RequestResult.Rejected, 15)]
            );
            await barrier();
            assert.equal(codecCalls, 1, 'late physical reply reaches its original codec once');
            assert.equal(
              await caller,
              early,
              'late codec success or failure cannot change public terminal'
            );
            assert.equal(submissions, 1);
          } finally {
            runtime.close();
            requests?.close();
          }
        });
      }
    }
  }
}

for (const terminal of ['normal', 'hook-failure', 'cancel', 'deadline', 'shutdown', 'lifecycle']) {
  for (const codec of ['valid', 'malformed', 'throwing', 'failed']) {
    test(`PREPARE physical receiver/${terminal}/${codec}`, async (t) => {
      const {
        ZLinkHostServiceRelocationRuntime
      } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
      const {
        ZLinkNodeRawMeshBackend
      } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
      const control = require('../../packages/framework/dist/runtime/host/service-relocation-control');
      const {
        encodeActorAuthorityIdentity
      } = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
      let now = 1000;
      let preserves = 0;
      t.mock.method(performance, 'now', () => now);
      t.mock.timers.enable({ apis: ['setTimeout'] });
      const requests = new RequestRuntime();
      const physical = deferred(),
        hook = deferred();
      const signal = new AbortController();
      const cause = new Error('public terminal');
      let hookSignal,
        entry,
        prepare,
        targetCommitted = false,
        submissions = 0,
        codecCalls = 0,
        hookCalls = 0,
        completions = 0;
      const events = [],
        routeObservers = new Set(),
        intentObservers = new Set();
      const decode = control.decodeServiceRelocationControlResponse;
      const codecFailure = new Error('PREPARE codec failure');
      t.mock.method(control, 'decodeServiceRelocationControlResponse', (...args) => {
        codecCalls++;
        if (codec === 'throwing') throw codecFailure;
        return decode(...args);
      });
      const backend = new ZLinkNodeRawMeshBackend(
        'mesh',
        'source',
        {},
        undefined,
        undefined,
        undefined,
        undefined,
        {
          raw: {
            topology: {
              localDescriptor: () => ({
                nodeRoutingId: 'source',
                meshName: 'mesh',
                state: 'serving',
                advertisedEndpoint: 'inproc://source',
                lifecycleGeneration: 2n,
                descriptorRevision: 1n,
                channels: []
              }),
              peers: () => []
            },
            mailbox: { pendingMessages: () => 0, pendingBytes: () => 0 },
            observePeerRoute: () => ({ ready: true, generation: 1n }),
            observePeerRouteChanged(listener) {
              routeObservers.add(listener);
              return () => routeObservers.delete(listener);
            },
            observePeerConnectionIntentRemoved(listener) {
              intentObservers.add(listener);
              return () => intentObservers.delete(listener);
            },
            requestService(
              _target,
              _frames,
              remainingMs,
              _admitted,
              pending,
              receiver,
              onSubmitted
            ) {
              events.push('binding');
              assert.equal(
                routeObservers.size,
                0,
                'readiness observer is detached before binding start'
              );
              assert.equal(remainingMs, 50);
              entry = pending;
              prepare = control.decodeServiceRelocationControlRequest(Buffer.from(_frames[0]));
              submissions++;
              assert.equal(receiver, undefined);
              onSubmitted?.();
              return physical.promise;
            },
            sendService(_target, frames) {
              const message = control.decodeServiceRelocationControlRequest(Buffer.from(frames[0]));
              if (message.kind === 'state') {
                events.push('hook');
                return hook.promise;
              }
              assert.equal(message.kind, 'cutover');
              targetCommitted = true;
              return Promise.resolve(true);
            }
          }
        }
      );
      const requestControl = backend.requestInfrastructureControlFrames.bind(backend);
      backend.requestInfrastructureControlFrames = (targetRid, frames, options) =>
        requestControl(targetRid, frames, {
          ...options,
          onSubmitted: (capability) => {
            hookCalls++;
            hookSignal = capability;
            return options.onSubmitted(capability);
          }
        });
      const actor = { context: { actorId: 'actor-1', meshName: 'mesh' } };
      const state = {
        actorId: 'actor-1',
        actorType: 'Player',
        actor,
        meshName: 'mesh',
        spotId: 'source-room',
        spotMembershipEpoch: 8n,
        nativeActorRef: { actorId: 'actor-1', generation: 5n, nodeRid: 'source' }
      };
      const target = {
        rid: 'target',
        lifecycleGeneration: 6n,
        ownerId: 'target-owner',
        leaseGeneration: 14n,
        applicationVersion: 4n,
        entrySpotId: 'entry-target'
      };
      const sourceAuthority = {
        kind: 'snapshot',
        storeVersion: { value: 'store-v17' },
        payload: encodeActorAuthorityIdentity({
          actorType: 'Player',
          actor: { actorId: 'actor-1', objectGeneration: 5n, nodeRid: 'source', meshName: 'mesh' },
          meshName: 'mesh',
          ownerNodeGeneration: 2n,
          owner: { ownerId: 'source-owner', leaseGeneration: 3n },
          spotId: 'source-room',
          spotGeneration: 4n,
          spotKind: framework.ZLinkSpotKind.User
        }),
        objectGeneration: 5n,
        authorityOwnerGeneration: 11n,
        ownerId: 'source-owner',
        ownerLeaseGeneration: 3n,
        allocation: {
          state: 'active',
          objectKind: 'actor',
          stableType: 'Player',
          descriptor: { meshName: 'mesh', rid: 'source' },
          descriptorLifecycleGeneration: 2n,
          capacity: { actors: 1, spots: 0 }
        },
        storeNow: new Date()
      };
      const targetAuthority = {
        ...sourceAuthority,
        storeVersion: { value: 'target-v18' },
        authorityOwnerGeneration: 12n,
        ownerId: target.ownerId,
        ownerLeaseGeneration: target.leaseGeneration,
        payload: encodeActorAuthorityIdentity({
          actorType: 'Player',
          actor: { actorId: 'actor-1', objectGeneration: 5n, nodeRid: 'target', meshName: 'mesh' },
          meshName: 'mesh',
          ownerNodeGeneration: target.lifecycleGeneration,
          owner: { ownerId: target.ownerId, leaseGeneration: target.leaseGeneration },
          spotId: 'target-room',
          spotGeneration: 9n,
          spotKind: framework.ZLinkSpotKind.User
        }),
        allocation: {
          ...sourceAuthority.allocation,
          descriptor: { meshName: 'mesh', rid: 'target' },
          descriptorLifecycleGeneration: 6n
        }
      };
      const runtime = new ZLinkHostServiceRelocationRuntime({
        requests,
        meshNode: () => backend,
        registration: {
          spotNodes: new Map([
            [
              'mesh',
              {
                actorFactoryRegistrations: {
                  Player: { implementation: class Player {}, relocation: { kind: 'recreate' } }
                }
              }
            ]
          ])
        },
        liveDescriptors: async () => [target],
        locationStore: () => ({
          readAuthority: async () => (targetCommitted ? targetAuthority : sourceAuthority),
          readOwnerLease: async () => ({
            kind: 'found',
            token: { ownerId: 'source-owner', leaseGeneration: 3n },
            leaseExpiresAt: new Date(Date.now() + 60_000),
            storeNow: new Date()
          }),
          compareExchangeAuthority: async (_key, expected, mutation) => {
            assert.equal(expected, sourceAuthority.storeVersion);
            assert.equal(mutation.generationTransition, 'preserve');
            preserves++;
            return { ...sourceAuthority, kind: 'stored' };
          }
        }),
        actorTransfer: {
          async prepareMaintenanceSession() {
            return {
              handoffBacklog: [],
              takeRelocationRelay: () => [],
              setReplayResults() {},
              async commit() {},
              async rollback() {},
              discard() {}
            };
          },
          async completeRelocationSourceLeave() {}
        },
        actorManager: () => ({
          detachRelocationSource() {},
          async completeCoreRelocationSource() {}
        })
      });
      try {
        const caller = observe(
          runtime.relocateActorJoin({
            meshName: 'mesh',
            actor,
            state,
            target: {
              routerChannelId: 'mesh',
              targetNodeRid: 'target',
              spotId: 'target-room',
              spotKind: framework.ZLinkSpotKind.User,
              targetSpotGeneration: 9n,
              targetNodeGeneration: 6n
            },
            relocationId: '00000000-0000-0000-0000-000000000091',
            deadlineAtMs: 1050,
            signal: signal.signal
          })
        );
        void caller.then(() => completions++);
        await barrier();
        assert.deepEqual(events, ['binding', 'hook']);
        assert.equal(requests.registry.size, 1);
        assert.equal(entry.deadlineMs, 1050);
        assert.equal(submissions, 1);
        assert.equal(hookCalls, 1);
        for (const observer of routeObservers) observer('target');
        const early = terminal !== 'normal';
        if (terminal === 'hook-failure') hook.reject(cause);
        if (terminal === 'cancel') signal.abort(cause);
        if (terminal === 'shutdown') requests.close(cause);
        if (terminal === 'lifecycle') for (const observer of intentObservers) observer('target');
        if (terminal === 'deadline') {
          now += 50;
          t.mock.timers.tick(50);
        }
        let before;
        if (early) {
          await barrier();
          now = entry.deadlineMs;
          t.mock.timers.tick(50);
          await barrier();
          t.mock.timers.tick(10);
          before = await caller;
          assert(before.error);
          if (['hook-failure', 'cancel', 'shutdown'].includes(terminal))
            assert.equal(before.error, cause);
          if (terminal === 'deadline')
            assert.equal(before.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
          assert.equal(codecCalls, 0);
          assert.equal(requests.registry.size, 0);
          assert.equal(hookSignal.aborted, true);
          assert.equal(intentObservers.size, 0);
        }
        const ready = {
          kind: 'ready',
          relocation: prepare.relocation,
          targetAttemptGeneration: prepare.targetAttemptGeneration,
          coordinator: prepare.coordinator,
          target: prepare.target,
          object: prepare.object,
          senderRole: 'target'
        };
        physical.resolve(
          codec === 'malformed'
            ? [Buffer.from('malformed')]
            : [
                control.encodeServiceRelocationControlResponse(
                  codec === 'failed' ? { ...ready, kind: 'failed', failureCode: 17 } : ready
                )
              ]
        );
        if (!early && ['malformed', 'throwing'].includes(codec)) {
          await barrier();
          now = entry.deadlineMs;
          t.mock.timers.tick(50);
          await barrier();
          t.mock.timers.tick(10);
        }
        const result = await caller;
        await barrier();
        assert.equal(
          codecCalls,
          1,
          'PREPARE retains its typed decoder after public or hook failure'
        );
        if (early) assert.equal(result, before);
        else if (codec === 'valid') {
          assert.deepEqual(result.value.actorRef, {
            actorId: 'actor-1',
            generation: 5n,
            nodeRid: 'target'
          });
          assert.equal(result.value.membershipEpoch, 8n);
          assert.equal(result.value.spotGeneration, 9n);
          await result.value.sourceCleanup();
        } else if (codec === 'throwing') assert.equal(result.error, codecFailure);
        else assert(result.error);
        assert.equal(
          preserves,
          early || ['malformed', 'throwing'].includes(codec) ? 1 : 0,
          'late decoding cannot authorize a second source restoration'
        );
        assert.equal(hookSignal.aborted, true, 'reply terminal stops a stalled hook');
        hook.reject(new Error('hook rejected after fixed public outcome'));
        await barrier();
        assert.equal(await caller, result);
        assert.equal(submissions, 1);
        assert.equal(hookCalls, 1);
        assert.equal(completions, 1);
        assert.equal(requests.registry.size, 0);
        assert.equal(intentObservers.size, 0);
      } finally {
        physical.resolve([Buffer.from('cleanup after fixed outcome')]);
        hook.reject(new Error('fixture cleanup'));
        requests.close();
        await runtime.dispose();
      }
    });
  }
}

for (const shared of [false, true]) {
  for (const kind of ['userSpotCreate', 'boundSessionClose']) {
    for (const queued of [false, true]) {
      for (const malformed of [false, true]) {
        test(`local physical receiver/${shared ? 'shared' : 'standalone'}/${kind}/queued=${queued}/malformed=${malformed}`, async (t) => {
          const {
            ServiceStatefulRuntime
          } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
          const { withTestSessionOwners } = require('./fixtures/session-owner-descriptors');
          const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
          const {
            RequestResult
          } = require('../../packages/framework/dist/runtime/backend/runtime-values');
          let now = 1000;
          t.mock.method(performance, 'now', () => now);
          t.mock.timers.enable({ apis: ['setTimeout'] });
          let timers = 0;
          const setTimer = globalThis.setTimeout;
          t.mock.method(globalThis, 'setTimeout', (...args) => {
            timers++;
            return setTimer(...args);
          });
          const requests = shared ? new RequestRuntime() : undefined;
          const permit = deferred();
          let closes = 0,
            handlers = 0,
            codecCalls = 0,
            finish,
            correlation;
          const raw = {
            setServiceIngress() {},
            reserveLocalIngress: () => permit.promise
          };
          const runtime = withTestSessionOwners(
            new ServiceStatefulRuntime(raw, 'source', 7n, undefined, requests)
          );
          const actor = runtime.createActor('local-actor').ref;
          if (kind === 'boundSessionClose')
            await runtime.bindSession('session', actor, 50, async () => true).promise;
          const decode = wire.decodeStatefulReply;
          t.mock.method(wire, 'decodeStatefulReply', (...args) => {
            codecCalls++;
            return decode(...args);
          });
          t.mock.method(runtime, 'ingress', async (record) => {
            handlers++;
            finish = record.reply;
            correlation = record.requestSequence;
            return 'infrastructure';
          });
          try {
            const operation =
              kind === 'boundSessionClose'
                ? runtime.closeBoundSession(actor, 1n, 50).promise
                : runtime.requestUserSpotOperation(
                    'source',
                    Buffer.alloc(8),
                    77n,
                    'userSpotCreate',
                    50
                  );
            const caller = observe(operation);
            await barrier();
            assert.equal(handlers, 0);
            if (!queued) {
              permit.resolve({
                close() {
                  closes++;
                }
              });
              await barrier();
              assert.equal(handlers, 1);
            }
            assert.equal(
              timers,
              shared ? 1 : 0,
              'only the existing shared registry scheduler may create a timer'
            );
            now += 50;
            if (shared) t.mock.timers.tick(50);
            else assert.equal(runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
            const early = await caller;
            assert(early.error instanceof framework.ZLinkFrameworkException);
            assert.equal(early.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
            assert.equal(runtime.pendingOperationCount, 0);
            assert.equal(codecCalls, 0);
            if (queued) {
              permit.resolve({
                close() {
                  closes++;
                }
              });
              await barrier();
              assert.equal(handlers, 0, 'expired queued operation cannot start local ingress');
              assert.equal(codecCalls, 0, 'no physical reply is synthesized before ingress');
            } else {
              finish(
                malformed
                  ? [Buffer.from('malformed')]
                  : [wire.encodeStatefulReply(correlation, RequestResult.Rejected, 15)]
              );
              await barrier();
              assert.equal(
                codecCalls,
                1,
                'started local ingress retains its typed physical decoder'
              );
            }
            assert.equal(closes, 1, 'acquired application owner is released exactly once');
            assert.equal(await caller, early);
          } finally {
            runtime.close();
            requests?.close();
          }
        });
      }
    }
  }
}

for (const destinationMode of [false, true]) {
  test(`typed Channel real mesh late physical codec1 close1/destination=${destinationMode}`, async (t) => {
    const {
      ZLinkRuntimeRouteTransport
    } = require('../../packages/framework/dist/runtime/channels/channel-transports');
    const {
      ZLinkMeshCompletionTable
    } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
    const {
      ZLinkNodeRawMeshBackend
    } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
    const {
      ServiceMailbox
    } = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
    const {
      ZLinkBufferMessage
    } = require('../../packages/framework/dist/runtime/backend/runtime-message');
    const {
      SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
    } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
    const runtime = new RequestRuntime();
    const controller = new AbortController();
    const entry = runtime.begin(1000, controller.signal);
    const table = new ZLinkMeshCompletionTable();
    let deliver,
      submissions = 0,
      codecs = 0,
      forwarded = 0;
    const raw = {
      mailbox: new ServiceMailbox(),
      close() {
        this.mailbox.close();
      }
    };
    const stateful = {
      entrySpot() {
        return { ref: { spotId: 'source', generation: 1n } };
      },
      requestToSpot(_source, _target, _payload, _timeout, admitted, pending, receiver) {
        submissions++;
        admitted?.();
        deliver = (reply) => receiver.operationCompleted(pending.id, reply, undefined);
        return pending;
      }
    };
    const backend = new ZLinkNodeRawMeshBackend(
      'mesh',
      'source',
      {},
      undefined,
      undefined,
      undefined,
      undefined,
      { raw, stateful }
    );
    const transport = new ZLinkRuntimeRouteTransport(
      () => undefined,
      undefined,
      () => ({ meshNode: () => backend, meshCompletionTable: () => table }),
      {
        serializers: new Map([
          [
            CONTENT_TYPE,
            {
              serialize(value) {
                return framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify(value)));
              },
              deserialize() {
                codecs++;
                return { answer: 42 };
              }
            }
          ]
        ])
      },
      undefined,
      undefined,
      undefined,
      () => false,
      undefined,
      runtime
    );
    const parts = encodeChannelReplyParts(
      {
        channelName: 'mesh',
        messageName: 'reply',
        correlationId: '0123456789abcdef0123456789abcdef'
      },
      { answer: 42 }
    ).map((part) => (typeof part === 'string' ? Buffer.from(part) : part));
    const header = JSON.parse(parts[0].toString());
    header.contentType = CONTENT_TYPE;
    parts[0] = Buffer.from(JSON.stringify(header));
    const payload = Buffer.alloc(4 + parts.reduce((size, part) => size + 4 + part.length, 0));
    payload.writeUInt32BE(parts.length, 0);
    let offset = 4;
    for (const part of parts) {
      payload.writeUInt32BE(part.length, offset);
      offset += 4;
      payload.set(part, offset);
      offset += part.length;
    }
    const closes = parts.map(() => 0);
    const close = ZLinkBufferMessage.prototype.close;
    t.mock.method(ZLinkBufferMessage.prototype, 'close', function () {
      const index = parts.findIndex((part) => part.equals(this.data()));
      if (index !== -1) closes[index]++;
      return close.call(this);
    });
    const destination = destinationMode
      ? {
          operationCompleted(_id, value) {
            forwarded++;
            entry.complete(value);
          },
          operationFailed(_id, error) {
            entry.fail(error);
          }
        }
      : undefined;
    try {
      const result = transport.requestToSpot(
        {
          routerChannelId: 'mesh',
          targetNodeRid: 'peer',
          spotId: 'spot',
          spotKind: framework.ZLinkSpotKind.User,
          targetSpotGeneration: 1n,
          targetNodeGeneration: 1n,
          authorityOwnerGeneration: 1n,
          targetOwnerId: 'owner',
          ownerLeaseGeneration: 1n,
          authorityStoreVersion: 'v1'
        },
        {},
        {
          packetName: 'Probe',
          timeoutMs: 1000,
          signal: controller.signal,
          requestOperation: entry
        },
        destination
      );
      const caller = observe(result.pending);
      const reason = new Error('original terminal before normal physical reply');
      controller.abort(reason);
      assert.equal((await caller).error, reason);
      deliver({
        terminalResult: 0,
        failureCode: 0,
        payload: { contentType: SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE, payload }
      });
      await barrier();
      assert.equal(submissions, 1);
      assert.equal(codecs, 1);
      assert.deepEqual(
        closes,
        parts.map(() => 1)
      );
      assert.equal(forwarded, destinationMode ? 1 : 0);
      assert.equal((await caller).error, reason);
      assert.equal(runtime.registry.size, 0);
      assert.equal(table.pendingCount, 0);
    } finally {
      runtime.close();
      table.dispose();
      raw.close();
    }
  });
}

for (const boundary of ['router', 'bound-router', 'named', 'bridge', 'local']) {
  for (const terminal of ['normal', 'failure']) {
    test(`destination Spot ${boundary}/${terminal} preserves branch timeout defaults`, async (t) => {
      const {
        ZLinkSpotRouteDispatchStrategy
      } = require('../../packages/framework/dist/runtime/channels/spot-route-dispatch-strategy');
      t.mock.method(performance, 'now', () => 0);
      const runtime = new RequestRuntime();
      const work = deferred();
      const cause = new Error('direct source failure');
      let entry,
        receiver,
        sourceCalls = 0;
      const target = {
        routerChannelId: 'route',
        targetNodeRid: 'peer',
        spotId: 'spot'
      };
      const registration = framework.createFrameworkRegistration({
        requestTimeoutMs: 50,
        routeChannels: [
          {
            routerChannelId: 'route',
            ...(boundary === 'bound-router' ? { bind: 'inproc://direct-default' } : {})
          }
        ],
        ...(boundary === 'named'
          ? { spotNodes: { route: { router: { bind: 'inproc://named-default' } } } }
          : {})
      });
      const routeSocket = {
        requestToSpot(_node, _spot, _parts, timeout, _admitted, operation, observer) {
          sourceCalls++;
          entry = operation;
          receiver = observer;
          assert.equal(timeout, boundary === 'named' ? 50 : undefined);
        }
      };
      const bridge = {
        request() {
          return {
            message() {
              return this;
            },
            timeout(value) {
              assert.equal(value, 50);
              return this;
            },
            submit(_admitted, operation, observer) {
              sourceCalls++;
              entry = operation;
              receiver = observer;
              return entry.promise;
            }
          };
        }
      };
      const strategy = new ZLinkSpotRouteDispatchStrategy({
        requests: runtime,
        registration,
        codecs: { serializers: new Map() },
        spotRouteBridges: new Map(boundary === 'bridge' ? [['route', bridge]] : []),
        flowCreationEnabled: () => false,
        sockets: {
          routeRouter() {
            return routeSocket;
          }
        },
        localSpotRouteDispatcher: {
          request(_spot, _packet, _value, context) {
            sourceCalls++;
            entry = context.requestOperation;
            return work.promise;
          }
        }
      });
      if (boundary === 'named')
        strategy.setSpotNodes(
          new Map([
            [
              'route',
              {
                routingId: 'other',
                entrySpot() {
                  return routeSocket;
                }
              }
            ]
          ])
        );
      if (boundary === 'local') strategy.setSpotNodes(new Map([['route', { routingId: 'peer' }]]));
      let delivered = 0;
      const pending = strategy.routeRequestToSpot(
        target,
        'Probe',
        {},
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        {
          operationCompleted(id, value) {
            delivered++;
            runtime.registry.handle(id).complete({ answer: value.answer + 1 });
          },
          operationFailed(id, error) {
            delivered++;
            runtime.registry.handle(id).fail(error);
          }
        }
      );
      const outcome = observe(pending);
      try {
        assert.equal(sourceCalls, 1);
        assert.equal(runtime.registry.size, 1);
        assert.equal(
          entry.deadlineMs,
          boundary === 'router' || boundary === 'bound-router' ? undefined : 50
        );
        assert.equal(pending, entry.promise);
        if (boundary === 'local') {
          if (terminal === 'normal') work.resolve({ answer: 42 });
          else work.reject(cause);
        } else if (terminal === 'normal')
          receiver.operationCompleted(
            entry.id,
            encodeChannelReplyParts(
              {
                channelName: 'route',
                messageName: 'Reply',
                correlationId: '0123456789abcdef0123456789abcdef'
              },
              { answer: 42 }
            ).map((part) =>
              require('../../packages/framework/dist/runtime/backend/runtime-message').ZLinkBufferMessage.from(
                part
              )
            ),
            undefined
          );
        else receiver.operationFailed(entry.id, cause, undefined);
        const result = await outcome;
        if (terminal === 'normal') assert.deepEqual(result.value, { answer: 43 });
        else assert.equal(result.error, cause);
        assert.equal(delivered, 1);
        assert.equal(runtime.registry.size, 0);
      } finally {
        work.reject(cause);
        await observe(work.promise);
        runtime.close();
      }
    });
  }
}

for (const cancelled of [false, true]) {
  test(`SourceSpot typed destination keeps physical decoder and original alias/cancel=${cancelled}`, async (t) => {
    const {
      ZLinkSourceSpotRouter
    } = require('../../packages/framework/dist/runtime/channels/source-spot-router');
    const {
      SPOT_DIRECT_ENVELOPE
    } = require('../../packages/framework/dist/runtime/channels/spot-direct-envelope');
    const {
      ZLinkBufferMessage
    } = require('../../packages/framework/dist/runtime/backend/runtime-message');
    const runtime = new RequestRuntime();
    const controller = new AbortController();
    const entry = runtime.begin(1000, controller.signal);
    const caller = observe(entry.promise);
    const router = new ZLinkSourceSpotRouter(1000, runtime);
    let physical,
      deliveries = 0;
    const source = {
      requestToSpot(_node, _spot, _parts, _timeout, _admitted, operation, receiver) {
        assert.equal(operation, entry);
        physical = receiver;
      }
    };
    const reply = ZLinkBufferMessage.from(
      JSON.stringify({ marker: SPOT_DIRECT_ENVELOPE, ok: true, response: { answer: 42 } })
    );
    const closed = t.mock.method(reply, 'close');
    const result = router.request(
      source,
      { routerChannelId: 'route', targetNodeRid: 'peer', spotId: 'spot' },
      'Probe',
      {},
      1000,
      controller.signal,
      undefined,
      entry,
      {
        operationCompleted(id, value) {
          deliveries++;
          assert.equal(id, entry.id);
          entry.complete(value);
        },
        operationFailed(_id, error) {
          entry.fail(error);
        }
      }
    );
    const reason = new Error('SourceSpot original cancellation');
    try {
      assert.equal(result, entry.promise);
      if (cancelled) {
        controller.abort(reason);
        assert.equal((await caller).error, reason);
      }
      physical.operationCompleted(entry.id, [reply], undefined);
      const actual = await caller;
      assert.equal(deliveries, 1);
      assert.equal(closed.mock.callCount(), 1);
      if (cancelled) assert.equal(actual.error, reason);
      else assert.deepEqual(actual.value, { answer: 42 });
      assert.equal(runtime.registry.size, 0);
    } finally {
      runtime.close();
    }
  });
}

for (const existing of [false, true]) {
  test(`prepared destination reports pre-abort without a second result/existing=${existing}`, async () => {
    const runtime = new RequestRuntime();
    const reason = new Error('already aborted destination');
    const signal = AbortSignal.abort(reason);
    const entry = existing ? runtime.begin(1000, signal) : undefined;
    let starts = 0,
      failures = 0;
    const result = runtime.prepare(
      1000,
      signal,
      () => {
        starts++;
      },
      (value) => value,
      (error) => error,
      entry,
      {
        operationCompleted() {
          assert.fail('no physical reply before submit');
        },
        operationFailed(_id, error) {
          failures++;
          assert.equal(error, reason);
        }
      }
    );
    try {
      assert.equal((await observe(result.pending)).error, reason);
      assert.equal(result.pending, result.operation.promise);
      if (existing) assert.equal(result.operation, entry);
      assert.equal(starts, 0);
      assert.equal(failures, 1);
      assert.equal(runtime.registry.size, 0);
    } finally {
      runtime.close();
    }
  });
}

test('prepared pre-aborted destination does not submit while existing workflow failure is queued', async () => {
  const runtime = new RequestRuntime();
  const entry = runtime.begin(1000);
  const reason = new Error('pre-aborted workflow');
  let starts = 0,
    failures = 0;
  const result = runtime.prepare(
    1000,
    AbortSignal.abort(reason),
    () => {
      starts++;
    },
    (value) => value,
    (error) => error,
    entry,
    {
      operationCompleted() {
        assert.fail('no submitted physical result');
      },
      operationFailed(_id, error) {
        failures++;
        queueMicrotask(() => entry.fail(error));
      }
    }
  );
  try {
    assert.equal(starts, 0);
    assert.equal(failures, 1);
    assert.equal(result.pending, entry.promise);
    assert.equal((await observe(result.pending)).error, reason);
    assert.equal(runtime.registry.size, 0);
  } finally {
    runtime.close();
  }
});
