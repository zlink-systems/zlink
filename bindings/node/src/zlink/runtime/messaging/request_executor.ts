// SPDX-License-Identifier: MPL-2.0

import { Message } from '../../contracts';
import { RequestError, RequestResult } from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { messageFromSnapshot } from './message_snapshot';

type Completion = { token: bigint; result: number; parts: Buffer[] | null };
interface Dispatcher {
  next: bigint;
  callbacks: Map<bigint, (result: number, parts: Buffer[] | null) => void>;
}

const dispatchers = new Map<unknown, Dispatcher>();

function dispatcher(handle: unknown): Dispatcher {
  let value = dispatchers.get(handle);
  if (value) return value;
  value = { next: 1n, callbacks: new Map() };
  dispatchers.set(handle, value);
  require('../native/native').requireNative().socketRequestCompletionHandler(
    handle,
    (item: Completion) => {
      const callback = value!.callbacks.get(item.token);
      if (!callback) return;
      value!.callbacks.delete(item.token);
      callback(item.result, item.parts);
    }
  );
  return value;
}

export function messagesFromNativeBuffers(
  buffers: readonly Buffer[] | null | undefined
): Message[] {
  return (buffers ?? []).map((buffer) =>
    messageFromSnapshot({ data: buffer ?? Buffer.alloc(0) })
  );
}

export function requestErrorFromResult(
  result: RequestResult,
  message: string
): RequestError {
  return withRuntimeErrorMessage(new RequestError(result, 0), message);
}

/**
 * Correlation is installed before the Core request submit. Core owns request
 * progress and invokes the reply callback exactly once; the binding only
 * resolves the Promise associated with the opaque token.
 */
export interface NativeRequestRegistration {
  readonly token: bigint;
  readonly promise: Promise<Message[]>;
  fail(error: unknown): boolean;
}

export interface NativeRequestCallbackRegistration {
  readonly token: bigint;
  fail(error: unknown): boolean;
  cancel(): void;
}

export function registerNativeRequestCallback(
  handle: unknown,
  callback: (error: Error | null, reply: Message[] | null) => void,
  requestErrorMessage: string
): NativeRequestCallbackRegistration {
  const state = dispatcher(handle);
  const token = state.next++;
  let settled = false;
  state.callbacks.set(token, (result, replyParts) => {
    if (settled) return;
    settled = true;
    if (result !== RequestResult.Ok) {
      callback(requestErrorFromResult(result as RequestResult, requestErrorMessage), null);
      return;
    }
    callback(null, messagesFromNativeBuffers(replyParts));
  });
  return {
    token,
    fail(error: unknown): boolean {
      if (settled) return false;
      settled = true;
      state.callbacks.delete(token);
      callback(error instanceof Error ? error : new Error(String(error)), null);
      return true;
    },
    cancel(): void {
      if (settled) return;
      settled = true;
      state.callbacks.delete(token);
    },
  };
}

export function registerNativeRequest(
  handle: unknown,
  requestErrorMessage: string
): NativeRequestRegistration {
  const state = dispatcher(handle);
  const token = state.next++;
  let settled = false;
  let resolvePromise!: (parts: Message[]) => void;
  let rejectPromise!: (error: unknown) => void;
  const promise = new Promise<Message[]>((resolve, reject) => {
    resolvePromise = resolve;
    rejectPromise = reject;
  });

  state.callbacks.set(token, (result, replyParts) => {
    if (settled) return;
    settled = true;
    if (result !== RequestResult.Ok) {
      rejectPromise(requestErrorFromResult(
        result as RequestResult,
        requestErrorMessage
      ));
      return;
    }
    resolvePromise(messagesFromNativeBuffers(replyParts));
  });

  return {
    token,
    promise,
    fail(error: unknown): boolean {
      if (settled) return false;
      settled = true;
      state.callbacks.delete(token);
      rejectPromise(error);
      return true;
    },
  };
}

/** Release isolate-local correlation state after Core has closed the socket. */
export function releaseNativeRequestDispatcher(handle: unknown): void {
  dispatchers.delete(handle);
}
