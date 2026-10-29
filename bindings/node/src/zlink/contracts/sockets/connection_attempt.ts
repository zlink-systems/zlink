// SPDX-License-Identifier: MPL-2.0

export const ConnectionAttemptState = Object.freeze({
  Connecting: 0,
  Connected: 1,
  AttemptFailed: 2
} as const);
export type ConnectionAttemptState =
  typeof ConnectionAttemptState[keyof typeof ConnectionAttemptState];

export const ConnectionFailureKind = Object.freeze({
  None: 0,
  Refused: 1,
  EndpointMissing: 2,
  TimedOut: 3,
  Unreachable: 4,
  NameResolution: 5,
  AuthFailed: 6,
  ProtocolFailed: 7,
  ConnectionLost: 8,
  Other: 9
} as const);
export type ConnectionFailureKind =
  typeof ConnectionFailureKind[keyof typeof ConnectionFailureKind];

export const ConnectionFailureStage = Object.freeze({
  None: 0,
  Resolve: 1,
  Transport: 2,
  Tls: 3,
  Websocket: 4,
  CoreHandshake: 5,
  Established: 6
} as const);
export type ConnectionFailureStage =
  typeof ConnectionFailureStage[keyof typeof ConnectionFailureStage];

/** Core's typed failure; errno and disconnect reason are diagnostic values. */
export interface ConnectionFailure {
  readonly kind: ConnectionFailureKind;
  readonly stage: ConnectionFailureStage;
  readonly normalizedErrno: number;
  readonly disconnectReason: number;
}

/** One current outbound registration. Compare opaque uint64 tokens only for equality. */
export interface ConnectionAttempt {
  readonly endpoint: string;
  readonly intentId: bigint;
  readonly revision: bigint;
  readonly lastFailureId: bigint;
  readonly state: ConnectionAttemptState;
  readonly lastFailure: ConnectionFailure;
}
