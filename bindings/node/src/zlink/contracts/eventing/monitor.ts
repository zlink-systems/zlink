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
  PeerWeightChanged: 0x8000,
  /** A remote-PAUSE condition was first applied to an affected application pipe. */
  SendFlowPaused: 0x10000,
  /** A remote-PAUSE condition was cleared on an affected application pipe. */
  SendFlowResumed: 0x20000,
  /** A stale or duplicate receive-flow-state frame (old generation or non-advancing epoch) was ignored. */
  FlowStateStale: 0x40000
} as const);
export type MonitorEventType = typeof MonitorEventType[keyof typeof MonitorEventType];

/**
 * Bits carried in `MonitorEvent.flags`. Values match
 * `ZLINK_MONITOR_EVENT_FLAG_*` in `zlink/eventing/api.h`.
 */
export const MonitorEventFlag = Object.freeze({
  /** Set on `ConnectionReady` when the connection changed from not-ready to ready. */
  ConnectionReadyEdge: 0x1,
  /**
   * Set on `SendFlowResumed` when clearing the remote pause left the pipe
   * actually writable. `value` carries the flow epoch.
   */
  SendFlowWritable: 0x2,
  /**
   * Set on `FlowStateStale` when the frame named a different connection
   * generation. `value` carries the received generation.
   */
  FlowStateStaleGeneration: 0x4,
  /**
   * Set on `FlowStateStale` when the epoch did not advance inside the
   * current generation. `value` carries the received epoch.
   */
  FlowStateStaleEpoch: 0x8
} as const);
export type MonitorEventFlag = typeof MonitorEventFlag[keyof typeof MonitorEventFlag];

/**
 * A snapshot of a monitored socket's state and auto-high-water-mark telemetry.
 *
 * `sourceKind` identifies the monitored source; `stateFlags`/`detailFlags` are
 * bit masks of its current state; `snd/rcvPendingMsgs` and
 * `snd/rcvPendingBytes` are queued message and byte counts;
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
  readonly sndPendingBytes: bigint;
  readonly rcvPendingBytes: bigint;
  readonly autoHwmEnabled: boolean;
  readonly autoHwmProfile: number;
  readonly autoHwmRole: number;
  readonly autoHwmPolicyClass: number;
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
  /**
   * DEALER/ROUTER receive-flow telemetry for affected application pipes
   * (present since ABI 4). Populated only for DEALER/ROUTER sockets; other socket
   * types report zero for all five fields.
   */
  /** Current count of application pipes this socket sees as remote-PAUSED. */
  readonly flowPausedConnections: bigint;
  /** Total PAUSED transitions actually applied (never a stale/duplicate). */
  readonly flowPauseAppliedTotal: bigint;
  /** Total RUNNING transitions actually applied (never a stale/duplicate). */
  readonly flowResumeAppliedTotal: bigint;
  /** Total stale or duplicate flow-state frames ignored. */
  readonly flowStateStaleTotal: bigint;
  /** Duration of the most recently completed PAUSED interval, in milliseconds. 0 if none has completed yet. */
  readonly flowPauseDurationMs: bigint;
  /** Whether the monitored socket is in the ready state. */
  isReady(): boolean;
  /**
   * Whether `flowPausedConnections` and the other `flow*` fields are
   * populated (ABI 4+, paired DEALER/ROUTER sockets only). Mirrors
   * `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` in `zlink_enum.h`.
   */
  isFlowStateDetailPopulated(): boolean;
}

/** A single socket connection-lifecycle event reported by a monitor. */
export class MonitorEvent {
  /** The kind of lifecycle event. */
  readonly event: MonitorEventType;
  /**
   * An event-specific value, such as an error code, reconnect interval, or
   * (for the flow events) a generation/epoch counter. A full 64-bit unsigned
   * value; use `bigint` arithmetic to read it losslessly.
   */
  readonly value: bigint;
  /** The peer routing id, or null when the event carries none. */
  readonly routingId: RoutingId | null;
  /** The local endpoint address. */
  readonly localAddr: string;
  /** The remote endpoint address. */
  readonly remoteAddr: string;
  /** Process-local identity of the physical transport attempt. */
  readonly connectionId: bigint;
  /** The transport lane associated with the event: 0 Application, 1 Completion. */
  readonly transportLane: number;
  /** Event-specific flags, including the connection-ready edge flag. */
  readonly flags: number;

  private constructor() {
    throw new TypeError('MonitorEvent values are created by monitor recv operations');
  }
}

Object.freeze(MonitorEvent);

/** Observes a socket's connection lifecycle events and current status. */
export interface MonitorSocket {
  /** Receive the next monitor event, or null when `DontWait` is set and none is available. */
  recv(flags?: number): MonitorEvent | null;
  /** Return a snapshot of the monitored socket's current status. */
  status(): MonitorStatus;
  /** Close the monitor and release its resources. */
  close(): void;
}

/** Canonical socket-monitor name used by event-source APIs. */
export type SocketMonitor = MonitorSocket;
