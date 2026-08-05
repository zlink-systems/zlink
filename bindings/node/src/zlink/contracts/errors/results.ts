// SPDX-License-Identifier: MPL-2.0

/** The outcome of submitting a send or publish. */
export const SubmitResult = Object.freeze({
  Ok: 0,
  Backpressured: 1,
  NotConnected: 2,
  NotFound: 3,
  Terminated: 4,
  InvalidHandle: 5,
  InvalidArgument: 6,
  NotSupported: 7,
  InvalidState: 8,
  ThreadViolation: 9,
  OutOfMemory: 10,
  SeqExhausted: 11,
  InternalError: 12,
  NotAdmitted: 13
} as const);
export type SubmitResult = typeof SubmitResult[keyof typeof SubmitResult];

/** The outcome of a request, as delivered to a request callback. */
export const RequestResult = Object.freeze({
  Ok: 0,
  TimedOut: 101,
  NotFound: 102,
  Terminated: 103,
  ProtocolError: 104,
  InternalError: 105,
  Rejected: 106,
  Conflict: 107,
  Busy: 108,
  NotConnected: 109,
  InvalidArgument: 110,
  InvalidState: 111,
  NotSupported: 112,
  Backpressured: 113
} as const);
export type RequestResult = typeof RequestResult[keyof typeof RequestResult];

/** The outcome of a receive. */
export const RecvResult = Object.freeze({
  Ok: 0,
  NoData: 201,
  Busy: 202,
  Terminated: 203,
  InvalidHandle: 204,
  NotSupported: 205,
  InternalError: 206,
  BufferTooSmall: 207,
  InvalidState: 208
} as const);
export type RecvResult = typeof RecvResult[keyof typeof RecvResult];

/** The outcome of registering or running a callback handler. */
export const HandlerResult = Object.freeze({
  Ok: 0,
  InvalidArgument: 301,
  Busy: 302,
  NotSupported: 303,
  Deadlock: 304,
  InvalidHandle: 305,
  InternalError: 306
} as const);
export type HandlerResult = typeof HandlerResult[keyof typeof HandlerResult];

/** The outcome of closing a socket or resource. */
export const CloseResult = Object.freeze({
  Ok: 0,
  Busy: 401,
  Shutdown: 402,
  InvalidHandle: 403,
  InternalError: 404
} as const);
export type CloseResult = typeof CloseResult[keyof typeof CloseResult];

/** The outcome of binding to an endpoint. */
export const BindResult = Object.freeze({
  Ok: 0,
  InvalidArgument: 501,
  AddrInUse: 502,
  NotSupported: 503,
  InvalidHandle: 504,
  InternalError: 505
} as const);
export type BindResult = typeof BindResult[keyof typeof BindResult];

/** The outcome of connecting to an endpoint. */
export const ConnectResult = Object.freeze({
  Ok: 0,
  InvalidArgument: 601,
  NotSupported: 602,
  InvalidHandle: 603,
  InternalError: 604,
  NotFound: 605,
  Conflict: 606,
  Busy: 607,
  AuthFailed: 608
} as const);
export type ConnectResult = typeof ConnectResult[keyof typeof ConnectResult];

/** The outcome of reading or applying a configuration option. */
export const ConfigResult = Object.freeze({
  Ok: 0,
  InvalidHandle: 701,
  InvalidArgument: 702,
  NotSupported: 703,
  InternalError: 704,
  InvalidState: 705,
  NotFound: 706,
  Conflict: 707,
  BufferTooSmall: 708,
  Busy: 709
} as const);
export type ConfigResult = typeof ConfigResult[keyof typeof ConfigResult];
