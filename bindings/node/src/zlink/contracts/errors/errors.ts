// SPDX-License-Identifier: MPL-2.0

import {
  BindResult,
  CloseResult,
  ConfigResult,
  ConnectResult,
  HandlerResult,
  RecvResult,
  RequestResult,
  SubmitResult,
} from './results';
import { constants } from 'node:os';
export {
  BindResult,
  CloseResult,
  ConfigResult,
  ConnectResult,
  HandlerResult,
  RecvResult,
  RequestResult,
  SubmitResult,
} from './results';

/** Base class for all errors thrown by the zlink bindings; carries a result `code` and the underlying native errno. */
export class ZlinkError extends Error {
  readonly code: number;
  readonly nativeErrno: number;

  constructor(code: number, nativeErrno = 0) {
    super(`zlink error ${code}`);
    this.name = 'ZlinkError';
    this.code = code | 0;
    this.nativeErrno = nativeErrno | 0;
  }
}

class ResultError<TResult extends number> extends ZlinkError {
  readonly result: TResult;

  protected constructor(name: string, result: TResult, nativeErrno = 0) {
    super(result, nativeErrno);
    this.name = name;
    this.result = result;
  }
}

const EFSM = 156384763;
export const ETERM = 156384765;

function representativeRequestErrno(result: RequestResult): number {
  switch (result) {
    case RequestResult.Ok: return 0;
    case RequestResult.TimedOut: return constants.errno.ETIMEDOUT;
    case RequestResult.NotFound: return constants.errno.ENOENT;
    case RequestResult.Terminated: return ETERM;
    case RequestResult.ProtocolError: return constants.errno.EPROTO;
    case RequestResult.InternalError: return constants.errno.EIO;
    case RequestResult.Rejected: return constants.errno.EACCES;
    case RequestResult.Conflict: return constants.errno.EEXIST;
    case RequestResult.Busy: return constants.errno.EBUSY;
    case RequestResult.NotConnected: return constants.errno.ENOTCONN;
    case RequestResult.InvalidArgument: return constants.errno.EINVAL;
    case RequestResult.InvalidState: return EFSM;
    case RequestResult.NotSupported: return constants.errno.ENOTSUP;
    case RequestResult.Backpressured: return constants.errno.EAGAIN;
    default: return constants.errno.EIO;
  }
}

/** Thrown when submitting a send or publish fails. */
export class SubmitError extends ResultError<SubmitResult> {
  constructor(result: SubmitResult, nativeErrno = 0) {
    super('SubmitError', result, nativeErrno);
  }
}

/** Thrown when a request fails or its reply reports an error. */
export class RequestError extends ResultError<RequestResult> {
  constructor(result: RequestResult, nativeErrno = representativeRequestErrno(result)) {
    super('RequestError', result, nativeErrno);
  }
}

/** Thrown when receiving a message fails. */
export class RecvError extends ResultError<RecvResult> {
  constructor(result: RecvResult, nativeErrno = 0) {
    super('RecvError', result, nativeErrno);
  }
}

/** Thrown when registering or running a callback handler fails. */
export class HandlerError extends ResultError<HandlerResult> {
  constructor(result: HandlerResult, nativeErrno = 0) {
    super('HandlerError', result, nativeErrno);
  }
}

/** Thrown when closing a socket or resource fails. */
export class CloseError extends ResultError<CloseResult> {
  constructor(result: CloseResult, nativeErrno = 0) {
    super('CloseError', result, nativeErrno);
  }
}

/** Thrown when binding a socket to an endpoint fails. */
export class BindError extends ResultError<BindResult> {
  constructor(result: BindResult, nativeErrno = 0) {
    super('BindError', result, nativeErrno);
  }
}

/** Thrown when connecting a socket to an endpoint fails. */
export class ConnectError extends ResultError<ConnectResult> {
  constructor(result: ConnectResult, nativeErrno = 0) {
    super('ConnectError', result, nativeErrno);
  }
}

/** Thrown when reading or applying a configuration option fails. */
export class ConfigError extends ResultError<ConfigResult> {
  constructor(result: ConfigResult, nativeErrno = 0) {
    super('ConfigError', result, nativeErrno);
  }
}
