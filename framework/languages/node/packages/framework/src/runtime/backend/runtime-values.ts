import type { Message } from '../../contracts/Common/Message';

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

export type ZLinkBackendSendFlags = number;
export type ZLinkBackendRecvFlags = number;
export const ZLINK_BACKEND_SEND_NONE: ZLinkBackendSendFlags = 0;
export const ZLINK_BACKEND_RECV_DONT_WAIT: ZLinkBackendRecvFlags = 1;

export class ZLinkBackendResultError extends Error {
  constructor(
    readonly operation: 'request' | 'submit',
    readonly result: number,
    readonly nativeErrno?: number,
    options?: ErrorOptions
  ) {
    super(`Backend ${operation} failed with result ${result}.`, options);
    this.name = 'ZLinkBackendResultError';
  }
}

export function isZLinkBackendResultError(error: unknown): error is ZLinkBackendResultError {
  return error instanceof ZLinkBackendResultError;
}
export type ZLinkBackendMessageLike = Message | Buffer | Uint8Array | string;
export type ZLinkBackendRequestCallback = (
  result: RequestResult,
  parts: readonly Message[]
) => void;

export interface ZLinkBackendSendSubmitBuilder {
  message(message: ZLinkBackendMessageLike): ZLinkBackendSendSubmitBuilder;
  submit(): unknown;
}

export interface ZLinkBackendSendBuilder {
  message(message: ZLinkBackendMessageLike): ZLinkBackendSendSubmitBuilder;
}

export interface ZLinkBackendReceived {
  readonly parts: readonly Message[];
  readonly routingId: unknown | null;
  readonly spotId?: unknown;
  readonly requestSeq: bigint | null;
  send?(): ZLinkBackendSendBuilder;
  reply(): ZLinkBackendSendBuilder;
  close(): void;
}

export interface ZLinkBackendTopicMessage {
  readonly parts: readonly Message[];
  readonly routingId: unknown | null;
  readonly topic: string;
  close(): void;
}

export type ZLinkBackendMonitorEventType = number;
