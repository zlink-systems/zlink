// SPDX-License-Identifier: MPL-2.0

import { requireNative } from '../native/native';
import { RecvFlags, SendFlags } from '../../contracts/sockets/socket_constants';
import {
  RecvError,
  RecvResult,
  SubmitError,
  SubmitResult,
  ZlinkError
} from '../../contracts/errors/errors';
import { createError, type NativeErrorCategory } from './error_mapping';
import { withRuntimeErrorMessage } from './error_state';

export function readErrno(): number {
  const native = requireNative();
  return typeof native.errno === 'function' ? native.errno() as number : 0;
}

export function isWouldBlock(errno = readErrno()): boolean {
  return errno === 11;
}

export function nativeErrorMessage(error: unknown, fallbackMessage: string): string {
  return error instanceof Error && error.message ? error.message : fallbackMessage;
}

export function lastError(category: NativeErrorCategory, message: string): ZlinkError {
  return createError(category, readErrno(), message);
}

export function nativeCall<T>(
  category: NativeErrorCategory,
  fallbackMessage: string,
  fn: () => T
): T {
  try {
    return fn();
  } catch (error) {
    throw createError(category, readErrno(), nativeErrorMessage(error, fallbackMessage));
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
  const message = nativeErrorMessage(error, fallbackMessage);
  const errno = readErrno();
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
  const errno = readErrno();
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
