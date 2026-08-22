// SPDX-License-Identifier: MPL-2.0

import {
  type MonitorSourceKindValue,
  type MonitorStatus
} from '../../contracts/eventing';
import type { MonitorStatusRaw } from './monitor_raw';

const MONITOR_STATE_READY = 1 << 0;
const MONITOR_STATUS_DETAIL_FLOW_STATE = 1 << 5;

export function materializeMonitorStatus(raw: MonitorStatusRaw): MonitorStatus {
  return {
    abiVersion: raw.abiVersion,
    structSize: raw.structSize,
    sourceKind: raw.sourceKind as MonitorSourceKindValue,
    stateFlags: raw.stateFlags,
    detailFlags: raw.detailFlags,
    sndPendingMsgs: raw.sndPendingMsgs,
    rcvPendingMsgs: raw.rcvPendingMsgs,
    sndPendingBytes: raw.sndPendingBytes,
    rcvPendingBytes: raw.rcvPendingBytes,
    autoHwmEnabled: raw.autoHwmEnabled,
    autoHwmProfile: raw.autoHwmProfile,
    autoHwmRole: raw.autoHwmRole,
    autoHwmPolicyClass: raw.autoHwmPolicyClass,
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
    flowPausedConnections: raw.flowPausedConnections,
    flowPauseAppliedTotal: raw.flowPauseAppliedTotal,
    flowResumeAppliedTotal: raw.flowResumeAppliedTotal,
    flowStateStaleTotal: raw.flowStateStaleTotal,
    flowPauseDurationMs: raw.flowPauseDurationMs,
    isReady(): boolean {
      return (this.stateFlags & MONITOR_STATE_READY) !== 0;
    },
    isFlowStateDetailPopulated(): boolean {
      return (this.detailFlags & MONITOR_STATUS_DETAIL_FLOW_STATE) !== 0;
    }
  };
}
