// SPDX-License-Identifier: MPL-2.0

import {
  type MonitorSourceKindValue,
  type MonitorStatus
} from '../../contracts/eventing';
import type { MonitorStatusRaw } from './monitor_raw';

const MONITOR_STATE_READY = 1 << 0;

export function materializeMonitorStatus(raw: MonitorStatusRaw): MonitorStatus {
  return {
    abiVersion: raw.abiVersion,
    structSize: raw.structSize,
    sourceKind: raw.sourceKind as MonitorSourceKindValue,
    stateFlags: raw.stateFlags,
    detailFlags: raw.detailFlags,
    sndPendingMsgs: raw.sndPendingMsgs,
    rcvPendingMsgs: raw.rcvPendingMsgs,
    autoHwmEnabled: raw.autoHwmEnabled,
    autoHwmProfile: raw.autoHwmProfile,
    autoHwmRole: raw.autoHwmRole,
    autoHwmPolicyClass: raw.autoHwmPolicyClass,
    autoHwmUnitBudgetBytes: raw.autoHwmUnitBudgetBytes,
    autoHwmSizeCap: raw.autoHwmSizeCap,
    autoHwmSocketMessageSlots: raw.autoHwmSocketMessageSlots,
    autoHwmConnectionBucketEnabled: raw.autoHwmConnectionBucketEnabled,
    autoHwmConnectionBucketCount: raw.autoHwmConnectionBucketCount,
    autoHwmConnectionBucketIndex: raw.autoHwmConnectionBucketIndex,
    autoHwmConnectionBucketHwm4K: raw.autoHwmConnectionBucketHwm4K,
    autoHwmConnectionBucketHysteresisRetained:
      raw.autoHwmConnectionBucketHysteresisRetained,
    autoHwmEffectiveMessageBytes: raw.autoHwmEffectiveMessageBytes,
    autoHwmPlannedSndHwmBytes: raw.autoHwmPlannedSndHwmBytes,
    autoHwmPlannedRcvHwmBytes: raw.autoHwmPlannedRcvHwmBytes,
    autoHwmAppliedSndHwmBytes: raw.autoHwmAppliedSndHwmBytes,
    autoHwmAppliedRcvHwmBytes: raw.autoHwmAppliedRcvHwmBytes,
    autoHwmEffectiveSndBuf: raw.autoHwmEffectiveSndBuf ?? 0,
    autoHwmEffectiveRcvBuf: raw.autoHwmEffectiveRcvBuf ?? 0,
    autoHwmLastRecalcMs: raw.autoHwmLastRecalcMs,
    autoHwmLastRecalcReason: raw.autoHwmLastRecalcReason,
    autoHwmSendBlockedRatioPpm: raw.autoHwmSendBlockedRatioPpm,
    autoHwmDeferredSndHwmBytes: raw.autoHwmDeferredSndHwmBytes,
    autoHwmDeferredRcvHwmBytes: raw.autoHwmDeferredRcvHwmBytes,
    autoHwmDeferredSndHwmValid: raw.autoHwmDeferredSndHwmValid,
    autoHwmDeferredRcvHwmValid: raw.autoHwmDeferredRcvHwmValid,
    sndBytesInFlight: raw.sndBytesInFlight,
    rcvBytesInFlight: raw.rcvBytesInFlight,
    minimumCoreMessageChargeBytes: raw.minimumCoreMessageChargeBytes,
    oversizeMessageAdmissionCount: raw.oversizeMessageAdmissionCount,
    oversizeMessageAdmissionMaxBytes: raw.oversizeMessageAdmissionMaxBytes,
    isReady(): boolean {
      return (this.stateFlags & MONITOR_STATE_READY) !== 0;
    }
  };
}
