// SPDX-License-Identifier: MPL-2.0

export interface MonitorStatusRaw {
  abiVersion: number;
  structSize: number;
  sourceKind: number;
  stateFlags: number;
  detailFlags: number;
  sndPendingMsgs: bigint;
  rcvPendingMsgs: bigint;
  autoHwmEnabled: boolean;
  autoHwmProfile: number;
  autoHwmRole: number;
  autoHwmPolicyClass: number;
  autoHwmUnitBudgetBytes: bigint;
  autoHwmSizeCap: number;
  autoHwmSocketMessageSlots: bigint;
  autoHwmConnectionBucketEnabled: boolean;
  autoHwmConnectionBucketCount: number;
  autoHwmConnectionBucketIndex: number;
  autoHwmConnectionBucketHwm4K: number;
  autoHwmConnectionBucketHysteresisRetained: boolean;
  autoHwmEffectiveMessageBytes: bigint;
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
}

export interface MonitorEventValueRaw {
  event: number;
  value: number;
  routingId?: Buffer | null;
  local?: string;
  remote?: string;
  localAddr?: string;
  remoteAddr?: string;
}
