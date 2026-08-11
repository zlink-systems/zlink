// SPDX-License-Identifier: MPL-2.0

import { Message } from '../../contracts';
import { RequestError, RequestResult } from '../../contracts/errors/errors';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import { submitNativeError, submitOrBackpressure } from '../errors/native_errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { messageFromSnapshot } from './message_snapshot';

export type RequestCallback = (result: RequestResult, replyParts: Message[]) => void;

export interface NativeRequestOptions {
  handle: unknown;
  callbackOrTimeout?: RequestCallback | number;
  flagsOrTimeout?: SendFlags | number;
  maybeTimeout?: number;
  startProgress: () => () => void;
  invoke: (
    token: bigint,
    flags: SendFlags,
    timeoutMs: number
  ) => void;
  submitErrorMessage: string;
  requestErrorMessage: string;
  promiseTimeoutMayUseFlagsOrTimeout?: boolean;
}

type Completion = { token: bigint; result: number; parts: Buffer[] | null };
interface Dispatcher { next: bigint; callbacks: Map<bigint, (result: number, parts: Buffer[] | null) => void>; }
const dispatchers = new Map<unknown, Dispatcher>();
function dispatcher(handle: unknown): Dispatcher {
  let value = dispatchers.get(handle);
  if (value) return value;
  value = { next: 1n, callbacks: new Map() };
  dispatchers.set(handle, value);
  require('../native/native').requireNative().socketRequestCompletionHandler(handle, (items: Completion[]) => {
    for (const item of items) {
      const callback = value!.callbacks.get(item.token);
      if (!callback) continue;
      value!.callbacks.delete(item.token);
      callback(item.result, item.parts);
    }
  });
  return value;
}

export function normalizeCallbackFlagsAndTimeout(
  flagsOrTimeout?: SendFlags | number,
  maybeTimeout?: number,
): { flags: SendFlags; timeoutMs: number } {
  if (typeof maybeTimeout === 'number') {
    return {
      flags: (flagsOrTimeout ?? SendFlags.None) as SendFlags,
      timeoutMs: maybeTimeout | 0,
    };
  }
  if (typeof flagsOrTimeout === 'number') {
    if (flagsOrTimeout === SendFlags.None || flagsOrTimeout === SendFlags.DontWait) {
      return { flags: flagsOrTimeout as SendFlags, timeoutMs: 0 };
    }
    return { flags: SendFlags.None, timeoutMs: flagsOrTimeout | 0 };
  }
  return { flags: SendFlags.None, timeoutMs: 0 };
}

export function messagesFromNativeBuffers(buffers: readonly Buffer[] | null | undefined): Message[] {
  return (buffers ?? []).map((buffer) => messageFromSnapshot({ data: buffer ?? Buffer.alloc(0) }));
}

export function requestErrorFromResult(result: RequestResult, message: string): RequestError {
  return withRuntimeErrorMessage(new RequestError(result, 0), message);
}

function executeCallbackRequest(options: NativeRequestOptions, callback: RequestCallback): boolean {
  // HOT PATH: callback requests are the binding-level fast path. Keep this
  // separate from executePromiseRequest so saturated request/reply benchmarks
  // do not allocate a Promise for every in-flight request.
  const { flags, timeoutMs } = normalizeCallbackFlagsAndTimeout(
    options.flagsOrTimeout,
    options.maybeTimeout
  );
  const releaseProgress = options.startProgress();
  const state = dispatcher(options.handle);
  const token = state.next++;
  try {
    state.callbacks.set(token,
      (result, replyParts) => {
        releaseProgress();
        callback(result as RequestResult, messagesFromNativeBuffers(replyParts));
      });
    options.invoke(token, flags, timeoutMs);
    return true;
  } catch (error) {
    state.callbacks.delete(token);
    releaseProgress();
    return submitOrBackpressure(error, flags, options.submitErrorMessage);
  }
}

function executePromiseRequest(options: NativeRequestOptions): Promise<Message[]> {
  const timeoutMs = typeof options.callbackOrTimeout === 'number'
    ? options.callbackOrTimeout
    : options.promiseTimeoutMayUseFlagsOrTimeout
      ? options.flagsOrTimeout ?? 0
      : 0;

  return new Promise<Message[]>((resolve, reject) => {
    const releaseProgress = options.startProgress();
    const state = dispatcher(options.handle);
    const token = state.next++;
    try {
      state.callbacks.set(token,
        (result, replyParts) => {
          releaseProgress();
          if (result !== RequestResult.Ok) {
            reject(requestErrorFromResult(result as RequestResult, options.requestErrorMessage));
            return;
          }
          resolve(messagesFromNativeBuffers(replyParts));
        });
      options.invoke(token, SendFlags.None, timeoutMs | 0);
    } catch (error) {
      state.callbacks.delete(token);
      releaseProgress();
      reject(submitNativeError(error, SendFlags.None, options.submitErrorMessage));
    }
  });
}

export function executeNativeRequest(options: NativeRequestOptions): Promise<Message[]> | boolean {
  if (typeof options.callbackOrTimeout === 'function') {
    return executeCallbackRequest(options, options.callbackOrTimeout);
  }
  return executePromiseRequest(options);
}
