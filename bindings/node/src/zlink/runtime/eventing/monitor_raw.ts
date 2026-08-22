// SPDX-License-Identifier: MPL-2.0

export interface MonitorStatusRaw {
  abiVersion: number;
  structSize: number;
  sourceKind: number;
  stateFlags: number;
  detailFlags: number;
  sndPendingMsgs: bigint;
  rcvPendingMsgs: bigint;
  sndPendingBytes: bigint;
  rcvPendingBytes: bigint;
  autoHwmEnabled: boolean;
  autoHwmProfile: number;
  autoHwmRole: number;
  autoHwmPolicyClass: number;
  autoHwmPlannedSndHwmBytes: bigint;
  autoHwmPlannedRcvHwmBytes: bigint;
  autoHwmAppliedSndHwmBytes: bigint;
  autoHwmAppliedRcvHwmBytes: bigint;
  autoHwmEffectiveSndBuf?: number;
  autoHwmEffectiveRcvBuf?: number;
  autoHwmLastRecalcMs: bigint;
  autoHwmLastRecalcReason: number;
  autoHwmSendBlockedRatioPpm: number;
  autoHwmDeferredSndHwmBytes: bigint;
  autoHwmDeferredRcvHwmBytes: bigint;
  autoHwmDeferredSndHwmValid: boolean;
  autoHwmDeferredRcvHwmValid: boolean;
  sndBytesInFlight: bigint;
  rcvBytesInFlight: bigint;
  minimumCoreMessageChargeBytes: bigint;
  oversizeMessageAdmissionCount: bigint;
  oversizeMessageAdmissionMaxBytes: bigint;
  flowPausedConnections: bigint;
  flowPauseAppliedTotal: bigint;
  flowResumeAppliedTotal: bigint;
  flowStateStaleTotal: bigint;
  flowPauseDurationMs: bigint;
}

export interface MonitorEventValueRaw {
  event: number;
  value: bigint;
  routingId?: Buffer | null;
  local?: string;
  remote?: string;
  localAddr?: string;
  remoteAddr?: string;
  connectionId?: bigint;
  transportPairId?: bigint;
  transportPairGeneration?: bigint;
  transportLane?: number;
  flags?: number;
}
