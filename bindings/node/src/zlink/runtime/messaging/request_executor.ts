// SPDX-License-Identifier: MPL-2.0

import { Message } from '../../contracts';
import { RequestError, RequestResult } from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { messageFromSnapshot } from './message_snapshot';
import { startRequestProgress } from './request_progress';

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
    (items: Completion[]) => {
      for (const item of items) {
        const callback = value!.callbacks.get(item.token);
        if (!callback) continue;
        value!.callbacks.delete(item.token);
        callback(item.result, item.parts);
      }
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
 * Correlation is installed before an exact DONTWAIT request attempt. Progress
 * starts only after Core accepts the request; admission failure removes the
 * registration without exposing callback-style completion publicly.
 */
export interface NativeRequestRegistration {
  readonly token: bigint;
  readonly promise: Promise<Message[]>;
  activate(): void;
  fail(error: unknown): boolean;
}

export function registerNativeRequest(
  handle: unknown,
  requestErrorMessage: string
): NativeRequestRegistration {
  const state = dispatcher(handle);
  const token = state.next++;
  let settled = false;
  let active = false;
  let releaseProgress = (): void => {};
  let resolvePromise!: (parts: Message[]) => void;
  let rejectPromise!: (error: unknown) => void;
  const promise = new Promise<Message[]>((resolve, reject) => {
    resolvePromise = resolve;
    rejectPromise = reject;
  });

  state.callbacks.set(token, (result, replyParts) => {
    if (settled) return;
    settled = true;
    releaseProgress();
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
    activate(): void {
      if (settled || active) return;
      active = true;
      releaseProgress = startRequestProgress(handle);
    },
    fail(error: unknown): boolean {
      if (settled) return false;
      settled = true;
      state.callbacks.delete(token);
      releaseProgress();
      rejectPromise(error);
      return true;
    },
  };
}

/** Release isolate-local correlation state after Core has closed the socket. */
export function releaseNativeRequestDispatcher(handle: unknown): void {
  dispatchers.delete(handle);
}
