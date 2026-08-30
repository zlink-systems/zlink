// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

interface SubmitObservation {
  keys: string[];
  inlineCompletionKeys?: string[];
}

const submitObservations: SubmitObservation[] = [];
const callbackCompletionKeys: string[][] = [];
const addonPath = path.resolve(__dirname, '../../build/Release/zlink.node');
const rawNative = require(addonPath) as Record<string, (...args: unknown[]) => any>;
const rawSendAsync = rawNative.socketSendAsync;
const rawCompletionHandler = rawNative.socketSendCompletionHandler;

const observedNative = new Proxy<Record<string, unknown>>({}, {
  get(_target, property) {
    if (property === 'socketSendAsync') {
      return (...args: unknown[]) => {
        const result = Reflect.apply(rawSendAsync, rawNative, args) as {
          inlineCompletion?: Record<string, unknown>;
        };
        submitObservations.push({
          keys: Object.keys(result).sort(),
          inlineCompletionKeys: result.inlineCompletion
            ? Object.keys(result.inlineCompletion).sort()
            : undefined
        });
        return result;
      };
    }
    if (property === 'socketSendCompletionHandler') {
      return (socket: unknown, handler: (event: Record<string, unknown>) => void) =>
        Reflect.apply(rawCompletionHandler, rawNative, [
          socket,
          (event: Record<string, unknown>) => {
            callbackCompletionKeys.push(Object.keys(event).sort());
            handler(event);
          }
        ]);
    }
    return Reflect.get(rawNative, property);
  }
});

const addonModule = require.cache[require.resolve(addonPath)];
if (!addonModule) throw new Error('native addon cache entry is missing');
addonModule.exports = observedNative;

const zlink = require('@zlink-systems/zlink');

function endpoint(label: string): string {
  return `inproc://node-send-completion-boundary-${label}-${process.pid}`;
}

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function within<T>(promise: Promise<T>, timeoutMs = 1_000): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`operation did not settle within ${timeoutMs}ms`)),
      timeoutMs
    );
    promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); }
    );
  });
}

function closeAll(context: any, ...sockets: any[]): void {
  for (const socket of sockets.reverse()) {
    try { socket.close(); } catch { /* cleanup should not mask the assertion */ }
  }
  context.close();
}

function assertSubmitShape(observation: SubmitObservation): void {
  const expected = observation.inlineCompletionKeys
    ? ['inlineCompletion', 'nativeErrno', 'result']
    : ['nativeErrno', 'result'];
  assert.deepEqual(observation.keys, expected);
}

const completionShape = ['result', 'terminalErrno', 'token'];

test('native send completion boundary exposes only Promise terminal fields', async () => {
  const directContext = zlink.createContext();
  const directSender = zlink.createPairSocket(directContext);
  const directReceiver = zlink.createPairSocket(directContext);
  try {
    directSender.bind(endpoint('direct'));
    directReceiver.connect(directSender.options.lastEndpoint);
    submitObservations.length = 0;
    callbackCompletionKeys.length = 0;

    await directSender.send().message('direct').submit();

    assert.equal(submitObservations.length, 1);
    assertSubmitShape(submitObservations[0]);
    const directCompletion = submitObservations[0].inlineCompletionKeys
      ?? callbackCompletionKeys[0];
    assert.deepEqual(directCompletion, completionShape);
  } finally {
    closeAll(directContext, directSender, directReceiver);
  }

  const deferredContext = zlink.createContext();
  deferredContext.options.autoHwmEnabled = false;
  const deferredSender = zlink.createPairSocket(deferredContext);
  const deferredReceiver = zlink.createPairSocket(deferredContext);
  deferredSender.options.sendHwm = 4_096n;
  deferredReceiver.options.recvHwm = 4_096n;
  const first = zlink.Message.from(Buffer.alloc(4_096, 0x61));
  const pending = zlink.Message.from(Buffer.alloc(4_096, 0x62));
  try {
    deferredSender.bind(endpoint('deferred'));
    deferredReceiver.connect(deferredSender.options.lastEndpoint);
    await deferredSender.send().message(first).submit();
    submitObservations.length = 0;
    callbackCompletionKeys.length = 0;

    const terminal = deferredSender.send().message(pending).timeout(-1).submit();
    await nextTurn();
    assert.equal(submitObservations.length, 1);
    assert.equal(submitObservations[0].inlineCompletionKeys, undefined);
    assertSubmitShape(submitObservations[0]);

    deferredSender.close();
    await assert.rejects(
      within(terminal),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Terminated
    );
    assert.deepEqual(callbackCompletionKeys, [completionShape]);
  } finally {
    closeAll(deferredContext, deferredSender, deferredReceiver);
    first.close();
    pending.close();
  }
});
