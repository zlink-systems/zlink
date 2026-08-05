import { loadBinding } from '../node-backend-adapter';
import type { ZLinkBackendObject } from '../contracts';
import { ZLinkBackendResultError } from '../runtime-values';

export type ZLinkBindingModule = typeof import('@zlink-systems/zlink');
export const zlink = loadBinding() as ZLinkBindingModule;

export type ZLinkBindingOperation = { [key: string]: (...args: unknown[]) => unknown };

export interface ZLinkBindingSendOperation {
  message(message: unknown): ZLinkBindingSendOperation;
  flags(flags: number): { submit(): boolean };
}

export interface ZLinkBindingRequestOperation {
  message(message: unknown): ZLinkBindingRequestSubmitOperation;
}

interface ZLinkBindingRequestSubmitOperation {
  message(message: unknown): ZLinkBindingRequestSubmitOperation;
  timeout(timeoutMs: number): ZLinkBindingRequestSubmitOperation;
  flags(flags: number): { submit(callback: unknown): boolean };
  submit(callback: unknown): boolean;
}

export interface ZLinkBindingPromiseRequestSubmitOperation<TParts extends unknown[]> {
  submit(callback: (result: number, parts: TParts) => void): boolean;
}

export function withNativeFallback<TAdapter extends object, TNative extends object>(
  adapter: TAdapter,
  nativeInstance: TNative
): TAdapter & TNative {
  return new Proxy(adapter as TAdapter & TNative, {
    get(target, property, receiver) {
      if (Reflect.has(target, property)) {
        const value = Reflect.get(target, property, receiver);
        return typeof value === 'function' ? value.bind(target) : value;
      }
      const value = Reflect.get(nativeInstance, property, nativeInstance);
      return typeof value === 'function' ? value.bind(nativeInstance) : value;
    },
    set(target, property, value, receiver) {
      return Reflect.has(target, property)
        ? Reflect.set(target, property, value, receiver)
        : Reflect.set(nativeInstance, property, value, nativeInstance);
    }
  });
}

export function unwrapBackendObject(value: unknown): unknown {
  if (typeof value === 'object' && value !== null) {
    const nativeInstance = (value as Partial<ZLinkBackendObject>).nativeInstance;
    if (nativeInstance !== undefined) {
      return nativeInstance;
    }
  }
  return value;
}

export function closeBindingMessages(messages: readonly { close(): void }[]): void {
  for (const message of messages) {
    message.close();
  }
}

export function waitForBindingOperation<T>(
  pending: Promise<T>,
  signal: AbortSignal | undefined,
  releaseLateResult: (result: T) => void
): Promise<T> {
  if (signal === undefined) {
    return pending;
  }
  signal.throwIfAborted();
  return new Promise<T>((resolve, reject) => {
    let completed = false;
    const onAbort = () => {
      if (completed) {
        return;
      }
      completed = true;
      reject(signal.reason);
    };
    signal.addEventListener('abort', onAbort, { once: true });
    pending.then(
      result => {
        if (completed) {
          releaseLateResult(result);
          return;
        }
        completed = true;
        signal.removeEventListener('abort', onAbort);
        resolve(result);
      },
      error => {
        if (completed) {
          return;
        }
        completed = true;
        signal.removeEventListener('abort', onAbort);
        reject(error);
      }
    );
  });
}

export function submitSendOperation(operation: unknown, payload: unknown, flags: number): boolean {
  let current = appendOperationParts(operation, payload);
  if (flags !== 0 && hasOperationMethod(current, 'flags')) {
    current = current.flags(flags) as ZLinkBindingOperation;
  }
  if (!hasOperationMethod(current, 'submit')) {
    throw new TypeError('Binding send operation does not expose submit().');
  }
  return Boolean(current.submit());
}

export function submitActorSendOperation(
  target: unknown,
  actor: unknown,
  payload: unknown,
  flags: number
): Promise<boolean> {
  const parts = Array.isArray(payload) ? payload : [payload];
  if (parts.length === 0) {
    throw new TypeError('Binding operation payload must contain at least one part.');
  }
  return new Promise<boolean>((resolve, reject) => {
    try {
      const accepted = (target as {
        sendToActorCallback(
          actor: unknown,
          parts: readonly unknown[],
          callback: (result: number, replyParts: readonly unknown[]) => void,
          flags: number,
          timeoutMs: number
        ): boolean;
      }).sendToActorCallback(
        actor,
        parts,
        result => {
          if (result === zlink.RequestResult.Ok) {
            resolve(true);
          } else {
            reject(new zlink.RequestError(result as typeof zlink.RequestResult[keyof typeof zlink.RequestResult], 0));
          }
        },
        flags,
        0
      );
      if (!accepted) {
        resolve(false);
      }
    } catch (error) {
      reject(error);
    }
  });
}

export function submitRequestOperation(
  operation: unknown,
  payload: unknown,
  callback: unknown,
  flags: number,
  timeoutMs?: number
): boolean {
  let current = appendOperationParts(operation, payload);
  if (timeoutMs !== undefined && timeoutMs > 0 && hasOperationMethod(current, 'timeout')) {
    current = current.timeout(timeoutMs) as ZLinkBindingOperation;
  }
  if (flags !== 0 && hasOperationMethod(current, 'flags')) {
    current = current.flags(flags) as ZLinkBindingOperation;
  }
  if (!hasOperationMethod(current, 'submit')) {
    throw new TypeError('Binding request operation does not expose submit().');
  }
  return Boolean(current.submit(callback));
}

export function submitPreparedRequestOperation(
  operation: unknown,
  callback: unknown,
  flags: number,
  timeoutMs?: number
): boolean {
  let current = operation;
  if (timeoutMs !== undefined && timeoutMs > 0 && hasOperationMethod(current, 'timeout')) {
    current = current.timeout(timeoutMs) as ZLinkBindingOperation;
  }
  if (flags !== 0 && hasOperationMethod(current, 'flags')) {
    current = current.flags(flags) as ZLinkBindingOperation;
  }
  if (!hasOperationMethod(current, 'submit')) {
    throw new TypeError('Binding request operation does not expose submit().');
  }
  return Boolean(current.submit(callback));
}

function appendOperationParts(operation: unknown, payload: unknown): ZLinkBindingOperation {
  const parts = Array.isArray(payload) ? payload : [payload];
  if (parts.length === 0) {
    throw new TypeError('Binding operation payload must contain at least one part.');
  }
  let current: unknown = operation;
  for (const part of parts) {
    if (!hasOperationMethod(current, 'message')) {
      throw new TypeError('Binding operation does not expose message().');
    }
    current = current.message(toNativeMessageLike(part));
  }
  return current as ZLinkBindingOperation;
}

export function hasOperationMethod(value: unknown, method: string): value is ZLinkBindingOperation {
  return typeof value === 'object' && value !== null &&
    typeof (value as { [key: string]: unknown })[method] === 'function';
}

export function isBindingNotFound(error: unknown): boolean {
  return error instanceof zlink.ConfigError && error.result === zlink.ConfigResult.NotFound;
}

function isNonBlockingRecvEmpty(error: unknown): boolean {
  return error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData;
}

export function isRouteRecvRetryable(error: unknown): boolean {
  return isNonBlockingRecvEmpty(error) ||
    (error instanceof zlink.RecvError &&
      (error.result === zlink.RecvResult.InternalError || error.result === zlink.RecvResult.InvalidHandle) &&
      isNativeBadAddress(error));
}

export function isPollerInterruptedError(error: unknown): boolean {
  return error instanceof zlink.RecvError && error.nativeErrno === 4;
}

function isNativeBadAddress(error: { nativeErrno?: unknown; message?: unknown }): boolean {
  return error.nativeErrno === 14 || /Bad address/i.test(String(error.message ?? ''));
}

export function submitBindingSend(
  operation: ZLinkBindingSendOperation,
  payload: unknown,
  flags: number
): boolean {
  try {
    let current = operation;
    if (Array.isArray(payload)) {
      for (const part of payload) {
        current = current.message(toNativeMessageLike(part));
      }
    } else {
      current = current.message(toNativeMessageLike(payload));
    }
    return current.flags(flags).submit();
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

export function submitBindingRequestCallback(
  operation: ZLinkBindingRequestOperation,
  payload: unknown,
  callback: unknown,
  flags: number,
  timeoutMs: number | undefined
): boolean {
  try {
    let current: ZLinkBindingRequestSubmitOperation | undefined;
    if (Array.isArray(payload)) {
      for (const part of payload) {
        const nativePart = toNativeMessageLike(part);
        current = current === undefined ? operation.message(nativePart) : current.message(nativePart);
      }
    } else {
      current = operation.message(toNativeMessageLike(payload));
    }
    current ??= operation.message(Buffer.alloc(0));
    if (timeoutMs !== undefined) {
      current = current.timeout(timeoutMs);
    }
    if (flags !== 0) {
      return current.flags(flags).submit(callback);
    }
    return current.submit(callback);
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

export function submitBindingRequest<TParts extends unknown[]>(
  operation: ZLinkBindingPromiseRequestSubmitOperation<TParts>
): Promise<TParts> {
  return new Promise((resolve, reject) => {
    const accepted = operation.submit((result, parts) => {
      if (result !== 0) {
        reject(new Error(`Binding request failed with result ${result}.`));
        return;
      }
      resolve(parts);
    });
    if (!accepted) {
      reject(new Error('Binding request submit was not accepted.'));
    }
  });
}

export async function closeWithBusyRetry(target: { close(): void }): Promise<void> {
  let lastError: unknown;
  for (let attempt = 0; attempt < 8; attempt++) {
    try {
      target.close();
      return;
    } catch (error) {
      if (isSuccessfulOrAlreadyShutdownCloseError(error)) {
        return;
      }
      if (!isBusyCloseError(error)) {
        throw error;
      }
      lastError = error;
      await new Promise<void>(resolve => setImmediate(resolve));
    }
  }
  throw lastError;
}

function isBusyCloseError(error: unknown): boolean {
  return error instanceof Error && 'code' in error && [401, 404].includes((error as { code: number }).code);
}

function isSuccessfulOrAlreadyShutdownCloseError(error: unknown): boolean {
  return isContextTerminatedError(error) || (
    error instanceof Error && 'code' in error && [0, 402, 403].includes((error as { code: number }).code)
  );
}

export function isContextTerminatedError(error: unknown): boolean {
  return error instanceof Error && /context was terminated/i.test(error.message);
}

export function disableSocketLinger(target: unknown): void {
  if (
    target !== null &&
    typeof target === 'object' &&
    'options' in target &&
    typeof target.options === 'object' &&
    target.options !== null &&
    'linger' in target.options
  ) {
    try {
      (target.options as { linger: number }).linger = 0;
    } catch (error) {
      if (!isContextTerminatedError(error)) {
        throw error;
      }
    }
  }
}

export function toNativeRoutingId(routingId: unknown): unknown {
  if (typeof routingId === 'string') return zlink.RoutingId.from(routingId);
  const toHex = (routingId as { readonly toHex?: unknown } | null)?.toHex;
  return typeof toHex === 'function'
    ? zlink.RoutingId.fromHex(toHex.call(routingId))
    : routingId;
}

function toNativeMessageLike(message: unknown): unknown {
  if (message instanceof zlink.Message || Buffer.isBuffer(message)
      || message instanceof Uint8Array || typeof message === 'string') {
    return message;
  }
  const data = (message as { readonly data?: unknown } | null)?.data;
  if (typeof data === 'function') {
    const bytes = data.call(message);
    if (bytes instanceof Uint8Array) return Buffer.from(bytes);
  }
  return message;
}

function translateBindingResultError(error: unknown): unknown {
  if (error instanceof zlink.SubmitError) {
    return new ZLinkBackendResultError('submit', error.result, error.nativeErrno, { cause: error });
  }
  if (error instanceof zlink.RequestError) {
    return new ZLinkBackendResultError('request', error.result, error.nativeErrno, { cause: error });
  }
  return error;
}

export function toNativeActorRef(actor: unknown): unknown {
  if (typeof actor !== 'object' || actor === null || !('nodeRid' in actor)) {
    return actor;
  }
  return {
    ...(actor as Record<string, unknown>),
    nodeRid: toNativeRoutingId((actor as { nodeRid: unknown }).nodeRid)
  };
}
