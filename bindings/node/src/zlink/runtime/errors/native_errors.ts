// SPDX-License-Identifier: MPL-2.0

import { constants } from 'node:os';
import { RecvFlags, SendFlags } from '../../contracts/sockets/socket_constants';
import {
  RecvError,
  RecvResult,
  SubmitError,
  SubmitResult
} from '../../contracts/errors/errors';
import { createError, type NativeErrorCategory } from './error_mapping';
import { withRuntimeErrorMessage } from './error_state';

/**
 * The errno of a failed Core call, read by the addon right after that
 * call returned. Errno is never read later: N-API and V8 work between the throw
 * and this catch may overwrite it. Any other failure has no errno (0).
 */
export function failureErrno(error: unknown): number {
  const nativeErrno = typeof error === 'object' && error !== null
    ? (error as { nativeErrno?: unknown }).nativeErrno
    : undefined;
  return typeof nativeErrno === 'number' ? nativeErrno : 0;
}

export function isWouldBlock(errno: number): boolean {
  return errno === constants.errno.EAGAIN;
}

export function nativeErrorMessage(error: unknown, fallbackMessage: string): string {
  return error instanceof Error && error.message ? error.message : fallbackMessage;
}

/**
 * The result Core returned with a failed call, when the addon reports one. Core
 * reaches some results only as a result (for example CONFIG_BUSY with EBUSY), so
 * an errno projection alone cannot recover them.
 */
export function failureResult(error: unknown): number | undefined {
  const nativeResult = typeof error === 'object' && error !== null
    ? (error as { nativeResult?: unknown }).nativeResult
    : undefined;
  return typeof nativeResult === 'number' ? nativeResult : undefined;
}

export function nativeCall<T>(
  category: NativeErrorCategory,
  fallbackMessage: string,
  fn: () => T
): T {
  try {
    return fn();
  } catch (error) {
    throw createError(
      category,
      failureErrno(error),
      nativeErrorMessage(error, fallbackMessage),
      failureResult(error)
    );
  }
}

export function bindCall<T>(fallbackMessage: string, fn: () => T): T {
  return nativeCall('bind', fallbackMessage, fn);
}

export function connectCall<T>(fallbackMessage: string, fn: () => T): T {
  return nativeCall('connect', fallbackMessage, fn);
}

export function configCall<T>(fallbackMessage: string, fn: () => T): T {
  return nativeCall('config', fallbackMessage, fn);
}

export function handlerCall<T>(fallbackMessage: string, fn: () => T): T {
  return nativeCall('handler', fallbackMessage, fn);
}

export function closeCall<T>(fallbackMessage: string, fn: () => T): T {
  return nativeCall('close', fallbackMessage, fn);
}

export function recvNativeError(
  error: unknown,
  flags: RecvFlags,
  fallbackMessage: string
): RecvError {
  if (error instanceof RecvError) return error;
  const message = nativeErrorMessage(error, fallbackMessage);
  const errno = failureErrno(error);
  if ((flags & RecvFlags.DontWait) !== 0 && isWouldBlock(errno)) {
    return withRuntimeErrorMessage(new RecvError(RecvResult.NoData, errno), message);
  }
  return createError('recv', errno, message) as RecvError;
}

export function submitNativeError(
  error: unknown,
  flags: SendFlags,
  fallbackMessage: string
): SubmitError {
  const message = nativeErrorMessage(error, fallbackMessage);
  const errno = failureErrno(error);
  const nativeResult = failureResult(error);
  if (nativeResult !== undefined) {
    return withRuntimeErrorMessage(
      new SubmitError(nativeResult as SubmitResult, errno),
      message
    );
  }
  if ((flags & SendFlags.DontWait) !== 0 && isWouldBlock(errno)) {
    return withRuntimeErrorMessage(new SubmitError(SubmitResult.Backpressured, errno), message);
  }
  return createError('submit', errno, message) as SubmitError;
}

export function submitOrBackpressure(
  error: unknown,
  flags: SendFlags,
  fallbackMessage: string
): false {
  const submitError = submitNativeError(error, flags, fallbackMessage);
  if (((flags | 0) & (SendFlags.DontWait | 0)) && submitError.result === SubmitResult.Backpressured) {
    return false;
  }
  throw submitError;
}
