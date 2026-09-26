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
} from '../../contracts/errors/errors';
import { withRuntimeErrorMessage } from './error_state';

export type NativeErrorCategory =
  | 'submit'
  | 'request'
  | 'recv'
  | 'handler'
  | 'close'
  | 'bind'
  | 'connect'
  | 'config';

// Linux errno values and the Core-defined errno values the addon reports.
const EPERM = 1;
const ENOENT = 2;
const EAGAIN = 11;
const ENOMEM = 12;
const EACCES = 13;
const EFAULT = 14;
const EBUSY = 16;
const EEXIST = 17;
const EINVAL = 22;
const EDEADLK = 35;
const EPROTO = 71;
const EOVERFLOW = 75;
const EMSGSIZE = 90;
const EPROTOTYPE = 91;
const EPROTONOSUPPORT = 93;
const ENOTSUP = 95;
const EADDRINUSE = 98;
const ENOBUFS = 105;
const ENOTCONN = 107;
const ESHUTDOWN = 108;
const ETIMEDOUT = 110;
const ECONNREFUSED = 111;
const EHOSTUNREACH = 113;
const EALREADY = 114;
const ESTALE = 116;
const ECANCELED = 125;
const EFSM = 156384763;
const ENOCOMPATPROTO = 156384764;
const ETERM = 156384765;
const EMTHREAD = 156384766;

export function isTerminationErrno(errno: number): boolean {
  return errno === ECANCELED || errno === ESHUTDOWN || errno === ETERM;
}

/**
 * Projects an errno onto the result enum of `category`. Each case follows the
 * Core projection table of the same result family
 * (`core/src/api/{core,message}/*_result_internal.hpp`) row for row.
 */
export function mapNativeErrno(category: NativeErrorCategory, errno: number): number {
  switch (category) {
    case 'submit':
      switch (errno) {
        case 0: return SubmitResult.Ok;
        case ENOTSUP: return SubmitResult.NotSupported;
        case EAGAIN:
        case ETIMEDOUT:
        case ENOBUFS: return SubmitResult.Backpressured;
        case ENOTCONN:
        case EHOSTUNREACH: return SubmitResult.NotConnected;
        case ECONNREFUSED:
        case EACCES:
        case EPROTOTYPE: return SubmitResult.NotAdmitted;
        case ENOENT: return SubmitResult.NotFound;
        case ESHUTDOWN:
        case ETERM: return SubmitResult.Terminated;
        case EFAULT: return SubmitResult.InvalidHandle;
        case EINVAL:
        case EMSGSIZE: return SubmitResult.InvalidArgument;
        case EFSM:
        case EBUSY:
        case ESTALE:
        case EALREADY: return SubmitResult.InvalidState;
        case EDEADLK:
        case EPERM:
        case EMTHREAD: return SubmitResult.ThreadViolation;
        case EOVERFLOW: return SubmitResult.SeqExhausted;
        case ENOMEM: return SubmitResult.OutOfMemory;
        default: return SubmitResult.InternalError;
      }
    case 'request':
      switch (errno) {
        case 0: return RequestResult.Ok;
        case ENOTSUP: return RequestResult.NotSupported;
        case EACCES:
        case ECONNREFUSED:
        case ECANCELED:
        case EPROTOTYPE: return RequestResult.Rejected;
        case ESTALE:
        case EEXIST: return RequestResult.Conflict;
        case EBUSY: return RequestResult.Busy;
        case ENOTCONN:
        case EHOSTUNREACH: return RequestResult.NotConnected;
        case EINVAL:
        case EFAULT: return RequestResult.InvalidArgument;
        case EFSM:
        case EALREADY: return RequestResult.InvalidState;
        case EAGAIN:
        case ENOBUFS: return RequestResult.Backpressured;
        case ESHUTDOWN:
        case ETERM: return RequestResult.Terminated;
        case ENOCOMPATPROTO:
        case EPROTO: return RequestResult.ProtocolError;
        case ETIMEDOUT: return RequestResult.TimedOut;
        case ENOENT: return RequestResult.NotFound;
        default: return RequestResult.InternalError;
      }
    case 'recv':
      switch (errno) {
        case 0: return RecvResult.Ok;
        case ENOTSUP: return RecvResult.NotSupported;
        case EAGAIN:
        case ETIMEDOUT: return RecvResult.NoData;
        case EBUSY: return RecvResult.Busy;
        case ETERM: return RecvResult.Terminated;
        case EFAULT: return RecvResult.InvalidHandle;
        case ENOBUFS: return RecvResult.BufferTooSmall;
        case EINVAL:
        case ESTALE:
        case ESHUTDOWN: return RecvResult.InvalidState;
        default: return RecvResult.InternalError;
      }
    case 'handler':
      switch (errno) {
        case 0: return HandlerResult.Ok;
        case ENOTSUP: return HandlerResult.NotSupported;
        case EINVAL: return HandlerResult.InvalidArgument;
        case EBUSY: return HandlerResult.Busy;
        case EDEADLK: return HandlerResult.Deadlock;
        case EFAULT: return HandlerResult.InvalidHandle;
        default: return HandlerResult.InternalError;
      }
    case 'close':
      switch (errno) {
        case 0: return CloseResult.Ok;
        case EBUSY:
        case EDEADLK: return CloseResult.Busy;
        case ESHUTDOWN: return CloseResult.Shutdown;
        case EFAULT:
        case ESTALE: return CloseResult.InvalidHandle;
        default: return CloseResult.InternalError;
      }
    case 'bind':
      switch (errno) {
        case 0: return BindResult.Ok;
        case ENOTSUP:
        case EPROTONOSUPPORT: return BindResult.NotSupported;
        case EINVAL: return BindResult.InvalidArgument;
        case EADDRINUSE: return BindResult.AddrInUse;
        case EFAULT: return BindResult.InvalidHandle;
        default: return BindResult.InternalError;
      }
    case 'connect':
      switch (errno) {
        case 0: return ConnectResult.Ok;
        case ENOTSUP:
        case EPROTONOSUPPORT: return ConnectResult.NotSupported;
        case EINVAL: return ConnectResult.InvalidArgument;
        case EFAULT: return ConnectResult.InvalidHandle;
        case ENOENT: return ConnectResult.NotFound;
        case EADDRINUSE:
        case EEXIST:
        case ESTALE: return ConnectResult.Conflict;
        case EBUSY:
        case ESHUTDOWN: return ConnectResult.Busy;
        case EACCES: return ConnectResult.AuthFailed;
        default: return ConnectResult.InternalError;
      }
    case 'config':
      switch (errno) {
        case 0: return ConfigResult.Ok;
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
}

/**
 * Builds the typed error of `category`. A result Core returned is used as it is;
 * otherwise the errno is projected through the Core table of that family.
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
