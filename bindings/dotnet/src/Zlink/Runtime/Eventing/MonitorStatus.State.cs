// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public sealed partial class MonitorStatus
{
    internal MonitorStatus(in ZlinkMonitorStatus native)
    {
        AbiVersion = native.AbiVersion;
        StructSize = native.StructSize;
        SourceKind = (MonitorSourceKind)native.MonitorSourceKind;
        StateFlags = (MonitorStateFlags)native.StateFlags;
        DetailFlags = (MonitorStatusDetailFlags)native.DetailFlags;
        SndPendingMsgs = native.SndPendingMsgs;
        RcvPendingMsgs = native.RcvPendingMsgs;
        SndPendingBytes = native.SndPendingBytes;
        RcvPendingBytes = native.RcvPendingBytes;
        AutoHwmEnabled = native.AutoHwmEnabled != 0;
        AutoHwmProfile = (AutoHwmProfile)native.AutoHwmProfile;
        AutoHwmRole = native.AutoHwmRole;
        AutoHwmPolicyClass = native.AutoHwmPolicyClass;
        AutoHwmPlannedSendHighWaterMarkBytes =
            native.AutoHwmPlannedSndHwmBytes;
        AutoHwmPlannedReceiveHighWaterMarkBytes =
            native.AutoHwmPlannedRcvHwmBytes;
        AutoHwmAppliedSendHighWaterMarkBytes =
            native.AutoHwmAppliedSndHwmBytes;
        AutoHwmAppliedReceiveHighWaterMarkBytes =
            native.AutoHwmAppliedRcvHwmBytes;
        AutoHwmEffectiveSndbuf = native.AutoHwmEffectiveSndbuf;
        AutoHwmEffectiveRcvbuf = native.AutoHwmEffectiveRcvbuf;
        AutoHwmLastRecalcMs = native.AutoHwmLastRecalcMs;
        AutoHwmLastRecalcReason =
            (AutoHwmRecalcReason)native.AutoHwmLastRecalcReason;
        AutoHwmSendBlockedRatioPpm = native.AutoHwmSendBlockedRatioPpm;
        AutoHwmDeferredSendHighWaterMarkBytes =
            native.AutoHwmDeferredSndHwmBytes;
        AutoHwmDeferredReceiveHighWaterMarkBytes =
            native.AutoHwmDeferredRcvHwmBytes;
        AutoHwmDeferredSendHighWaterMarkValid =
            native.AutoHwmDeferredSndHwmValid != 0;
        AutoHwmDeferredReceiveHighWaterMarkValid =
            native.AutoHwmDeferredRcvHwmValid != 0;
        SendBytesInFlight = native.SndBytesInFlight;
        ReceiveBytesInFlight = native.RcvBytesInFlight;
        MinimumCoreMessageChargeBytes = native.MinimumCoreMessageChargeBytes;
        OversizeMessageAdmissionCount = native.OversizeMessageAdmissionCount;
        OversizeMessageAdmissionMaxBytes =
            native.OversizeMessageAdmissionMaxBytes;
        FlowPausedConnections = native.FlowPausedConnections;
        FlowPauseAppliedTotal = native.FlowPauseAppliedTotal;
        FlowResumeAppliedTotal = native.FlowResumeAppliedTotal;
        FlowStateStaleTotal = native.FlowStateStaleTotal;
        FlowPauseDurationMs = native.FlowPauseDurationMs;
    }
}
