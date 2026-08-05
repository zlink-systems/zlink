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

/** Thrown when submitting a send or publish fails. */
export class SubmitError extends ResultError<SubmitResult> {
  constructor(result: SubmitResult, nativeErrno = 0) {
    super('SubmitError', result, nativeErrno);
  }
}

/** Thrown when a request fails or its reply reports an error. */
export class RequestError extends ResultError<RequestResult> {
  constructor(result: RequestResult, nativeErrno = 0) {
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
