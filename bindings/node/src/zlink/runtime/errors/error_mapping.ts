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

const ETERM = 156384765;

export type NativeErrorCategory =
  | 'submit'
  | 'request'
  | 'recv'
  | 'handler'
  | 'close'
  | 'bind'
  | 'connect'
  | 'config';

export function mapNativeErrno(category: NativeErrorCategory, errno: number): number {
  switch (category) {
    case 'submit':
      switch (errno) {
        case 0: return SubmitResult.Ok;
        case 11: return SubmitResult.Backpressured;
        case 107:
        case 113:
        case 110: return SubmitResult.NotConnected;
        case 111: return SubmitResult.NotAdmitted;
        case 2: return SubmitResult.NotFound;
        case 125:
        case ETERM: return SubmitResult.Terminated;
        case 14: return SubmitResult.InvalidHandle;
        case 22: return SubmitResult.InvalidArgument;
        case 95:
        case 93: return SubmitResult.NotSupported;
        case 16: return SubmitResult.InvalidState;
        case 35: return SubmitResult.ThreadViolation;
        case 12: return SubmitResult.OutOfMemory;
        case 75: return SubmitResult.SeqExhausted;
        default: return SubmitResult.InternalError;
      }
    case 'request':
      switch (errno) {
        case 0: return RequestResult.Ok;
        case 110: return RequestResult.TimedOut;
        case 2: return RequestResult.NotFound;
        case 125:
        case ETERM: return RequestResult.Terminated;
        case 111: return RequestResult.Rejected;
        case 17: return RequestResult.Conflict;
        case 16: return RequestResult.Busy;
        case 107:
        case 113: return RequestResult.NotConnected;
        case 22: return RequestResult.InvalidArgument;
        case 95:
        case 93: return RequestResult.NotSupported;
        default: return RequestResult.ProtocolError;
      }
    case 'recv':
      switch (errno) {
        case 0: return RecvResult.Ok;
        case 11: return RecvResult.NoData;
        case 4: return RecvResult.NoData;
        case 16: return RecvResult.Busy;
        case 125: return RecvResult.Terminated;
        case 14: return RecvResult.InvalidHandle;
        default: return RecvResult.InternalError;
      }
    case 'handler':
      switch (errno) {
        case 0: return HandlerResult.Ok;
        case 22: return HandlerResult.InvalidArgument;
        case 16: return HandlerResult.Busy;
        case 35: return HandlerResult.Deadlock;
        case 14: return HandlerResult.InvalidHandle;
        default: return HandlerResult.InternalError;
      }
    case 'close':
      switch (errno) {
        case 0: return CloseResult.Ok;
        case 16: return CloseResult.Busy;
        case 108: return CloseResult.Shutdown;
        case 14: return CloseResult.InvalidHandle;
        default: return CloseResult.InternalError;
      }
    case 'bind':
      switch (errno) {
        case 0: return BindResult.Ok;
        case 22: return BindResult.InvalidArgument;
        case 98: return BindResult.AddrInUse;
        case 95:
        case 93: return BindResult.NotSupported;
        case 14: return BindResult.InvalidHandle;
        default: return BindResult.InternalError;
      }
    case 'connect':
      switch (errno) {
        case 0: return ConnectResult.Ok;
        case 22: return ConnectResult.InvalidArgument;
        case 95:
        case 93: return ConnectResult.NotSupported;
        case 14: return ConnectResult.InvalidHandle;
        case 2: return ConnectResult.NotFound;
        case 17: return ConnectResult.Conflict;
        case 16: return ConnectResult.Busy;
        default: return ConnectResult.InternalError;
      }
    case 'config':
      switch (errno) {
        case 0: return ConfigResult.Ok;
        case 14: return ConfigResult.InvalidHandle;
        case 22: return ConfigResult.InvalidArgument;
        case 95:
        case 93: return ConfigResult.NotSupported;
        case 16: return ConfigResult.InvalidState;
        case 2: return ConfigResult.NotFound;
        default: return ConfigResult.InternalError;
      }
  }
}

export function createError(
  category: NativeErrorCategory,
  errno: number,
  message?: string
): ZlinkError {
  const code = mapNativeErrno(category, errno);
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
