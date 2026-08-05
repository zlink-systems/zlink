// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../core';

/** Identifies what a monitored source is. The Core raw API defines only the socket source. */
export const MonitorSourceKind = Object.freeze({ Socket: 1 } as const);
export type MonitorSourceKindValue = typeof MonitorSourceKind[keyof typeof MonitorSourceKind];

/** The kind of a delivered socket monitor connection-lifecycle event. */
export const MonitorEventType = Object.freeze({
  Connected: 0x0001,
  ConnectDelayed: 0x0002,
  ConnectRetried: 0x0004,
  Listening: 0x0008,
  BindFailed: 0x0010,
  Accepted: 0x0020,
  AcceptFailed: 0x0040,
  Closed: 0x0080,
  CloseFailed: 0x0100,
  Disconnected: 0x0200,
  MonitorStopped: 0x0400,
  HandshakeFailedNoDetail: 0x0800,
  ConnectionReady: 0x1000,
  HandshakeFailedProtocol: 0x2000,
  HandshakeFailedAuth: 0x4000,
  PeerWeightChanged: 0x8000
} as const);
export type MonitorEventType = typeof MonitorEventType[keyof typeof MonitorEventType];

/**
 * A snapshot of a monitored socket's state and auto-high-water-mark telemetry.
 *
 * `sourceKind` identifies the monitored source; `stateFlags`/`detailFlags` are
 * bit masks of its current state; `snd/rcvPendingMsgs` are queued message
 * counts; and the `autoHwm*` fields report the automatic high-water-mark sizing
 * decisions (applied marks, effective buffers, last recalculation, deferred
 * shrinks).
 */
export interface MonitorStatus {
  readonly abiVersion: number;
  readonly structSize: number;
  readonly sourceKind: MonitorSourceKindValue;
  readonly stateFlags: number;
  readonly detailFlags: number;
  readonly sndPendingMsgs: bigint;
  readonly rcvPendingMsgs: bigint;
  readonly autoHwmEnabled: boolean;
  readonly autoHwmProfile: number;
  readonly autoHwmRole: number;
  readonly autoHwmPolicyClass: number;
  readonly autoHwmUnitBudgetBytes: bigint;
  readonly autoHwmSizeCap: number;
  readonly autoHwmSocketMessageSlots: bigint;
  readonly autoHwmConnectionBucketEnabled: boolean;
  readonly autoHwmConnectionBucketCount: number;
  readonly autoHwmConnectionBucketIndex: number;
  readonly autoHwmConnectionBucketHwm4K: number;
  readonly autoHwmConnectionBucketHysteresisRetained: boolean;
  readonly autoHwmEffectiveMessageBytes: bigint;
  readonly autoHwmPlannedSndHwmBytes: bigint;
  readonly autoHwmPlannedRcvHwmBytes: bigint;
  readonly autoHwmAppliedSndHwmBytes: bigint;
  readonly autoHwmAppliedRcvHwmBytes: bigint;
  readonly autoHwmEffectiveSndBuf: number;
  readonly autoHwmEffectiveRcvBuf: number;
  readonly autoHwmLastRecalcMs: bigint;
  readonly autoHwmLastRecalcReason: number;
  readonly autoHwmSendBlockedRatioPpm: number;
  readonly autoHwmDeferredSndHwmBytes: bigint;
  readonly autoHwmDeferredRcvHwmBytes: bigint;
  readonly autoHwmDeferredSndHwmValid: boolean;
  readonly autoHwmDeferredRcvHwmValid: boolean;
  readonly sndBytesInFlight: bigint;
  readonly rcvBytesInFlight: bigint;
  readonly minimumCoreMessageChargeBytes: bigint;
  readonly oversizeMessageAdmissionCount: bigint;
  readonly oversizeMessageAdmissionMaxBytes: bigint;
  /** Whether the monitored socket is in the ready state. */
  isReady(): boolean;
}

/** A single socket connection-lifecycle event reported by a monitor. */
export class MonitorEvent {
  /** The kind of lifecycle event. */
  readonly event: MonitorEventType;
  /** An event-specific value, such as an error code or reconnect interval. */
  readonly value: number;
  /** The peer routing id, or null when the event carries none. */
  readonly routingId: RoutingId | null;
  /** The local endpoint address. */
  readonly localAddr: string;
  /** The remote endpoint address. */
  readonly remoteAddr: string;

  private constructor() {
    throw new TypeError('MonitorEvent values are created by monitor recv operations');
  }
}

Object.freeze(MonitorEvent);

/** Observes a socket's connection lifecycle events and current status. */
export interface MonitorSocket {
  /** Receive the next monitor event, or null when `DontWait` is set and none is available. */
  recv(flags?: number): MonitorEvent | null;
  /** Register a callback invoked for each monitor event on a background dispatch thread. */
  onEvent(handler: (event: MonitorEvent) => void): void;
  /** Return a snapshot of the monitored socket's current status. */
  status(): MonitorStatus;
  /** Close the monitor and release its resources. */
  close(): void;
}
