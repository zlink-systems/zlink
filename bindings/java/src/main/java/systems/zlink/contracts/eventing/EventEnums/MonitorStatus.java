/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.sockets.AutoHwmRecalcReason;
import java.util.EnumSet;

/**
 * A snapshot of a monitored socket's state and auto-high-water-mark telemetry.
 * @param abiVersion native snapshot ABI version
 * @param structSize native snapshot structure size in bytes
 * @param sourceKind what kind of source is being monitored
 * @param stateFlags bitmask of current state flags
 * @param detailFlags bitmask of detail flags
 * @param sndPendingMsgs pending outbound message count
 * @param rcvPendingMsgs pending inbound message count
 * @param autoHwmEnabled whether auto-HWM is enabled
 * @param autoHwmProfile the current auto-HWM sizing profile
 * @param autoHwmRole the socket's auto-HWM role
 * @param autoHwmPolicyClass the auto-HWM policy class
 * @param autoHwmUnitBudgetBytes the per-message unit budget in bytes
 * @param autoHwmSizeCap the computed auto-HWM size cap
 * @param autoHwmSocketMessageSlots the socket's message slot capacity
 * @param autoHwmConnectionBucketEnabled whether a connection bucket applied
 * @param autoHwmConnectionBucketCount the peer count used by the bucket planner
 * @param autoHwmConnectionBucketIndex the selected bucket index
 * @param autoHwmConnectionBucketHwm4K the selected bucket HWM for 4 KiB messages
 * @param autoHwmConnectionBucketHysteresisRetained whether hysteresis retained the previous bucket
 * @param autoHwmEffectiveMessageBytes the effective message size in bytes
 * @param autoHwmPlannedSendHwmBytes planned send HWM in accounted bytes
 * @param autoHwmPlannedRecvHwmBytes planned receive HWM in accounted bytes
 * @param autoHwmAppliedSendHwmBytes applied send HWM in accounted bytes
 * @param autoHwmAppliedRecvHwmBytes applied receive HWM in accounted bytes
 * @param autoHwmAppliedSndBuffer the applied send buffer size
 * @param autoHwmAppliedRcvBuffer the applied receive buffer size
 * @param autoHwmLastRecalcMs when the last recalculation occurred, in milliseconds
 * @param autoHwmLastRecalcReason what triggered the last recalculation
 * @param autoHwmSendBlockedRatioPpm the send-blocked ratio in parts per million
 * @param autoHwmDeferredSendHwmBytes deferred send HWM in accounted bytes
 * @param autoHwmDeferredRecvHwmBytes deferred receive HWM in accounted bytes
 * @param autoHwmDeferredSendHwmValid whether the deferred send value is valid
 * @param autoHwmDeferredRecvHwmValid whether the deferred receive value is valid
 * @param sendBytesInFlight bytes retained by outbound pipe directions
 * @param recvBytesInFlight bytes retained by inbound pipe directions
 * @param minimumCoreMessageChargeBytes minimum charge for one Core frame
 * @param oversizeMessageAdmissionCount empty-pipe oversize admission count
 * @param oversizeMessageAdmissionMaxBytes largest admitted oversize message
 */
public record MonitorStatus(int abiVersion, int structSize,
                              MonitorSourceKind sourceKind,
                              EnumSet<MonitorStateFlags> stateFlags,
                              EnumSet<MonitorStatusDetailFlags> detailFlags,
                              long sndPendingMsgs, long rcvPendingMsgs,
                              boolean autoHwmEnabled,
                              AutoHwmProfile autoHwmProfile,
                              int autoHwmRole, int autoHwmPolicyClass,
                              long autoHwmUnitBudgetBytes,
                              int autoHwmSizeCap,
                              long autoHwmSocketMessageSlots,
                              boolean autoHwmConnectionBucketEnabled,
                              int autoHwmConnectionBucketCount,
                              int autoHwmConnectionBucketIndex,
                              int autoHwmConnectionBucketHwm4K,
                              boolean autoHwmConnectionBucketHysteresisRetained,
                              long autoHwmEffectiveMessageBytes,
                              long autoHwmPlannedSendHwmBytes,
                              long autoHwmPlannedRecvHwmBytes,
                              long autoHwmAppliedSendHwmBytes,
                              long autoHwmAppliedRecvHwmBytes,
                              int autoHwmAppliedSndBuffer,
                              int autoHwmAppliedRcvBuffer,
                              long autoHwmLastRecalcMs,
                              AutoHwmRecalcReason autoHwmLastRecalcReason,
                              int autoHwmSendBlockedRatioPpm,
                              long autoHwmDeferredSendHwmBytes,
                              long autoHwmDeferredRecvHwmBytes,
                              boolean autoHwmDeferredSendHwmValid,
                              boolean autoHwmDeferredRecvHwmValid,
                              long sendBytesInFlight,
                              long recvBytesInFlight,
                              long minimumCoreMessageChargeBytes,
                              long oversizeMessageAdmissionCount,
                              long oversizeMessageAdmissionMaxBytes) {
    public boolean isReady() {
        return stateFlags.contains(MonitorStateFlags.READY);
    }
}
