// SPDX-License-Identifier: MPL-2.0

import {
  BindError,
  BindResult,
  CloseError,
  CloseResult,
  ConfigError,
  ConfigResult,
  ConnectError,
  ConnectResult,
  HandlerError,
  HandlerResult,
  RecvError,
  RecvResult,
  RequestError,
  RequestResult,
  SubmitError,
  SubmitResult,
  ZlinkError,
  ETERM,
} from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from './error_state';
import { constants } from 'node:os';

export type NativeErrorCategory =
  | 'submit'
  | 'request'
  | 'recv'
  | 'handler'
  | 'close'
  | 'bind'
  | 'connect'
  | 'config';

// Platform errno values are resolved at runtime: pointer-returning constructors
// have no Core result.
const ENOENT = constants.errno.ENOENT;
const EFAULT = constants.errno.EFAULT;
const EBUSY = constants.errno.EBUSY;
const EEXIST = constants.errno.EEXIST;
const EINVAL = constants.errno.EINVAL;
const EPROTO = constants.errno.EPROTO;
const EMSGSIZE = constants.errno.EMSGSIZE;
const ENOTSUP = constants.errno.ENOTSUP;
const ENOBUFS = constants.errno.ENOBUFS;
const ENOTCONN = constants.errno.ENOTCONN;
// os.constants.errno has no ESHUTDOWN. Linux and Darwin use the system value;
// Windows uses Core's ZLINK_HAUSNUMERO + 22 (core/include/zlink_errno.h).
const ESHUTDOWN = process.platform === 'win32' ? 156384712 + 22
  : process.platform === 'darwin' ? 58 : 108;
const ETIMEDOUT = constants.errno.ETIMEDOUT;
const EALREADY = constants.errno.EALREADY;
// Windows has no ESTALE; Core defines it as ZLINK_HAUSNUMERO + 19.
const ESTALE = constants.errno.ESTALE ?? 156384712 + 19;
const ECANCELED = constants.errno.ECANCELED;

export function isTerminationErrno(errno: number): boolean {
  return errno === ECANCELED || errno === ESHUTDOWN || errno === ETERM;
}

/**
 * Pointer-returning constructors have no Core result, so their errno classifies
 * a configuration failure. Every other family carries the Core result; without
 * one the failure is an internal error.
 */
export function mapNativeErrno(category: NativeErrorCategory, errno: number): number {
  if (category === 'config') {
    switch (errno) {
      case 0: return ConfigResult.InternalError;
      case ENOTSUP: return ConfigResult.NotSupported;
      case EFAULT: return ConfigResult.InvalidHandle;
      case EINVAL:
      case EMSGSIZE: return ConfigResult.InvalidArgument;
      case EBUSY:
      case ESHUTDOWN:
      case ESTALE:
      case EALREADY:
      case ENOTCONN:
      case ETIMEDOUT:
      case EPROTO: return ConfigResult.InvalidState;
      case ENOENT: return ConfigResult.NotFound;
      case EEXIST: return ConfigResult.Conflict;
      case ENOBUFS: return ConfigResult.BufferTooSmall;
      default: return ConfigResult.InternalError;
    }
  }
  switch (category) {
    case 'submit': return SubmitResult.InternalError;
    case 'request': return RequestResult.InternalError;
    case 'recv': return RecvResult.InternalError;
    case 'handler': return HandlerResult.InternalError;
    case 'close': return CloseResult.InternalError;
    case 'bind': return BindResult.InternalError;
    case 'connect': return ConnectResult.InternalError;
  }
}

/**
 * Builds the typed error of `category`, preserving Core's result when present.
 */
export function createError(
  category: NativeErrorCategory,
  errno: number,
  message?: string,
  result?: number
): ZlinkError {
  const code = result ?? mapNativeErrno(category, errno);
  switch (category) {
    case 'submit':
      return withRuntimeErrorMessage(new SubmitError(code as SubmitResult, errno), message);
    case 'request':
      return withRuntimeErrorMessage(new RequestError(code as RequestResult, errno), message);
    case 'recv':
      return withRuntimeErrorMessage(new RecvError(code as RecvResult, errno), message);
    case 'handler':
      return withRuntimeErrorMessage(new HandlerError(code as HandlerResult, errno), message);
    case 'close':
      return withRuntimeErrorMessage(new CloseError(code as CloseResult, errno), message);
    case 'bind':
      return withRuntimeErrorMessage(new BindError(code as BindResult, errno), message);
    case 'connect':
      return withRuntimeErrorMessage(new ConnectError(code as ConnectResult, errno), message);
    case 'config':
      return withRuntimeErrorMessage(new ConfigError(code as ConfigResult, errno), message);
  }
}
