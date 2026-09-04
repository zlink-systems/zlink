// SPDX-License-Identifier: MPL-2.0

/** A socket's messaging pattern (PAIR, PUB, DEALER, ROUTER, STREAM, and so on). */
export const SocketType = Object.freeze({
  ANY: 0,
  PAIR: 0x1001, PUB: 0x1002, SUB: 0x1003, DEALER: 0x1004,
  ROUTER: 0x1005, XPUB: 0x1006, XSUB: 0x1007, STREAM: 0x1008,
  Any: 0,
  Pair: 0x1001,
  Pub: 0x1002,
  Sub: 0x1003,
  Dealer: 0x1004,
  Router: 0x1005,
  XPub: 0x1006,
  XSub: 0x1007,
  Stream: 0x1008
} as const);
export type SocketTypeValue = typeof SocketType[keyof typeof SocketType];

/** A bitmask selecting which socket monitor events to subscribe to. */
export type MonitorEventMask = number;
/** Subscribe to every socket monitor event. */
export const SOCKET_MONITOR_EVENT_ALL = 0x7FFFF;

/**
 * A socket's local receive-flow state, applied to DEALER/ROUTER sockets via
 * `setReceiveFlowState`. Values match `zlink_receive_flow_state_t`. Setting
 * `PAUSED` synthesizes a remote-PAUSE condition for affected Application
 * pipes; control uses the Application connection for count-1 peers and the
 * Completion connection for count-2 ROUTER-ROUTER peers. Setting the same
 * state again is a no-op success.
 */
export const ReceiveFlowState = Object.freeze({
  RUNNING: 0,
  PAUSED: 1
} as const);
export type ReceiveFlowState = typeof ReceiveFlowState[keyof typeof ReceiveFlowState];

/** Flags that modify send behavior; `DontWait` reports back-pressure instead of blocking. */
export const SendFlags = Object.freeze({ None: 0, DontWait: 0x0001 } as const);
export type SendFlags = typeof SendFlags[keyof typeof SendFlags];

/** Flags that modify receive behavior; `DontWait` returns instead of blocking when no message is available. */
export const RecvFlags = Object.freeze({ None: 0, DontWait: 0x0001 } as const);
export type RecvFlags = typeof RecvFlags[keyof typeof RecvFlags];

/** Kinds carried by socket-local Core completion records. */
export const CompletionKind = Object.freeze({
  Send: 1,
  Request: 2,
  Writable: 3
} as const);
export type CompletionKind = typeof CompletionKind[keyof typeof CompletionKind];

/** How a socket reacts to a peer that reuses an existing routing id. */
export const RidDuplicatePolicy = Object.freeze({
  Reject: 0,
  Handover: 1
} as const);
export type RidDuplicatePolicy =
  typeof RidDuplicatePolicy[keyof typeof RidDuplicatePolicy];
export type RidDuplicatePolicyValue = RidDuplicatePolicy;

/** Whether a failed submit is retried (`Off` or `LocalFailure`). */
export const SubmitRetryMode = Object.freeze({
  Off: 0,
  LocalFailure: 1
} as const);
export type SubmitRetryMode =
  typeof SubmitRetryMode[keyof typeof SubmitRetryMode];
export type SubmitRetryModeValue = SubmitRetryMode;

/** STREAM receive representation. Unspecified cannot be assigned by callers. */
export const StreamRecvMode = Object.freeze({
  Unspecified: 0,
  Raw: 1,
  Packet: 2
} as const);
export type StreamRecvMode = typeof StreamRecvMode[keyof typeof StreamRecvMode];

/** Readiness conditions a poll source can be watched for or report (readable, writable, error). */
export const PollEventFlag = Object.freeze({
  PollIn: 1,
  PollOut: 2,
  PollErr: 4,
  PollPri: 8,
  PollCompletion: 32
} as const);
export type PollEventFlagValue =
  typeof PollEventFlag[keyof typeof PollEventFlag];
