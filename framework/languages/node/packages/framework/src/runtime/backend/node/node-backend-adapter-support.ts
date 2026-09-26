import { loadBinding } from '../node-backend-adapter';
import { SubmitResult, ZLinkBackendResultError } from '../runtime-values';
import { requireOneWayCompletion, ZLinkSubmitStatus } from '../../messaging/submission-result';

export type ZLinkBindingModule = typeof import('@zlink-systems/zlink');
export const zlink = loadBinding() as ZLinkBindingModule;

export type ZLinkBindingOperation = { [key: string]: (...args: unknown[]) => unknown };

export interface ZLinkBindingAsyncSendOperation {
  message(message: unknown): ZLinkBindingAsyncSendSubmitOperation;
}

export interface ZLinkBindingPublishOperation {
  message(message: unknown): ZLinkBindingPublishSubmitOperation;
}

export interface ZLinkBindingReplyOperation {
  message(message: unknown): ZLinkBindingReplySubmitOperation;
}

interface ZLinkBindingPublishSubmitOperation {
  message(message: unknown): ZLinkBindingPublishSubmitOperation;
  submit(): void;
}

interface ZLinkBindingReplySubmitOperation {
  message(message: unknown): ZLinkBindingReplySubmitOperation;
  submit(): void;
}

interface ZLinkBindingAsyncSendSubmitOperation {
  message(message: unknown): ZLinkBindingAsyncSendSubmitOperation;
  submit(): import('@zlink-systems/zlink').SendSubmission;
  submit_sync(): void;
}

export interface ZLinkBindingRequestOperation {
  message(message: unknown): ZLinkBindingRequestSubmitOperation;
}

interface ZLinkBindingRequestSubmitOperation {
  message(message: unknown): ZLinkBindingRequestSubmitOperation;
  timeout(timeoutMs: number): ZLinkBindingRequestSubmitOperation;
  submit(): import('@zlink-systems/zlink').RequestSubmission;
}

export function isBindingNotFound(error: unknown): boolean {
  return error instanceof zlink.ConfigError && error.result === zlink.ConfigResult.NotFound;
}

function isNonBlockingRecvEmpty(error: unknown): boolean {
  return error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData;
}

export function isRouteRecvRetryable(error: unknown): boolean {
  return (
    isNonBlockingRecvEmpty(error) ||
    (error instanceof zlink.RecvError &&
      (error.result === zlink.RecvResult.InternalError ||
        error.result === zlink.RecvResult.InvalidHandle) &&
      isNativeBadAddress(error))
  );
}

function isNativeBadAddress(error: { nativeErrno?: unknown; message?: unknown }): boolean {
  return error.nativeErrno === 14 || /Bad address/i.test(String(error.message ?? ''));
}

export function submitBindingPublish(
  operation: ZLinkBindingPublishOperation,
  payload: unknown
): void {
  try {
    let current: ZLinkBindingPublishSubmitOperation | undefined;
    const parts = Array.isArray(payload) ? payload : [payload];
    for (const part of parts.length === 0 ? [Buffer.alloc(0)] : parts) {
      const nativePart = toNativeMessageLike(part);
      current = current === undefined ? operation.message(nativePart) : current.message(nativePart);
    }
    current!.submit();
  } catch (error) {
    const translated = translateBindingResultError(error);
    if (
      translated instanceof ZLinkBackendResultError &&
      translated.operation === 'submit' &&
      (translated.result === SubmitResult.Backpressured ||
        translated.result === SubmitResult.NotAdmitted)
    ) {
      requireOneWayCompletion(
        { status: ZLinkSubmitStatus.Backpressured },
        'Classic fanout publish'
      );
    }
    throw translated;
  }
}

export function submitBindingReply(operation: ZLinkBindingReplyOperation, payload: unknown): void {
  try {
    let current: ZLinkBindingReplySubmitOperation | undefined;
    const parts = Array.isArray(payload) ? payload : [payload];
    for (const part of parts.length === 0 ? [Buffer.alloc(0)] : parts) {
      const nativePart = toNativeMessageLike(part);
      current = current === undefined ? operation.message(nativePart) : current.message(nativePart);
    }
    current!.submit();
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

export async function submitBindingAsyncSend(
  operation: ZLinkBindingAsyncSendOperation,
  payload: unknown
): Promise<void> {
  try {
    let current: ZLinkBindingAsyncSendSubmitOperation | undefined;
    const parts = Array.isArray(payload) ? payload : [payload];
    for (const part of parts) {
      const nativePart = toNativeMessageLike(part);
      current = current === undefined ? operation.message(nativePart) : current.message(nativePart);
    }
    current ??= operation.message(Buffer.alloc(0));
    await current.submit().admitted;
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

export function submitBindingSyncSend(
  operation: ZLinkBindingAsyncSendOperation,
  payload: unknown
): void {
  try {
    let current: ZLinkBindingAsyncSendSubmitOperation | undefined;
    const parts = Array.isArray(payload) ? payload : [payload];
    for (const part of parts) {
      const nativePart = toNativeMessageLike(part);
      current = current === undefined ? operation.message(nativePart) : current.message(nativePart);
    }
    current ??= operation.message(Buffer.alloc(0));
    current.submit_sync();
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

export async function submitBindingRequest(
  operation: ZLinkBindingRequestOperation,
  payload: unknown,
  timeoutMs: number | undefined
): Promise<readonly unknown[]> {
  try {
    let current: ZLinkBindingRequestSubmitOperation | undefined;
    if (Array.isArray(payload)) {
      for (const part of payload) {
        const nativePart = toNativeMessageLike(part);
        current =
          current === undefined ? operation.message(nativePart) : current.message(nativePart);
      }
    } else {
      current = operation.message(toNativeMessageLike(payload));
    }
    current ??= operation.message(Buffer.alloc(0));
    if (timeoutMs !== undefined) {
      current = current.timeout(timeoutMs);
    }
    return await current.submit().reply;
  } catch (error) {
    throw translateBindingResultError(error);
  }
}

/**
 * Closes a binding handle once. An already closed or terminated handle counts as
 * closed. A busy close propagates to the caller.
 */
export function closeBindingHandle(target: { close(): void }): void {
  try {
    target.close();
  } catch (error) {
    if (!isSuccessfulOrAlreadyShutdownCloseError(error)) throw error;
  }
}

function isSuccessfulOrAlreadyShutdownCloseError(error: unknown): boolean {
  return (
    isContextTerminatedError(error) ||
    (error instanceof Error &&
      'code' in error &&
      [0, 402, 403].includes((error as { code: number }).code))
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
  return typeof toHex === 'function' ? zlink.RoutingId.fromHex(toHex.call(routingId)) : routingId;
}

function toNativeMessageLike(message: unknown): unknown {
  if (
    message instanceof zlink.Message ||
    Buffer.isBuffer(message) ||
    message instanceof Uint8Array ||
    typeof message === 'string'
  ) {
    return message;
  }
  const data = (message as { readonly data?: unknown } | null)?.data;
  if (typeof data === 'function') {
    const bytes = data.call(message);
    if (bytes instanceof Uint8Array) return Buffer.from(bytes);
  }
  return message;
}

export function translateBindingResultError(error: unknown): unknown {
  if (error instanceof zlink.SubmitError) {
    return new ZLinkBackendResultError('submit', error.result, error.nativeErrno, { cause: error });
  }
  if (error instanceof zlink.RequestError) {
    return new ZLinkBackendResultError('request', error.result, error.nativeErrno, {
      cause: error
    });
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
