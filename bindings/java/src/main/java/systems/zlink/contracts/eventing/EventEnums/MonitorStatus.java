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
 * @param sndPendingBytes pending outbound byte count
 * @param rcvPendingBytes pending inbound byte count
 * @param autoHwmEnabled whether auto-HWM is enabled
 * @param autoHwmProfile the current auto-HWM sizing profile
 * @param autoHwmRole the socket's auto-HWM role
 * @param autoHwmPolicyClass the auto-HWM policy class
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
 * @param flowPausedConnections current count of application pipes seen as remote-PAUSED
 * @param flowPauseAppliedTotal total PAUSED transitions actually applied
 * @param flowResumeAppliedTotal total RUNNING transitions actually applied
 * @param flowStateStaleTotal total stale or duplicate flow-state frames ignored
 * @param flowPauseDurationMs duration of the most recently completed PAUSED interval, in milliseconds
 */
public record MonitorStatus(int abiVersion, int structSize,
                              MonitorSourceKind sourceKind,
                              EnumSet<MonitorStateFlags> stateFlags,
                              EnumSet<MonitorStatusDetailFlags> detailFlags,
                              long sndPendingMsgs, long rcvPendingMsgs,
                              long sndPendingBytes, long rcvPendingBytes,
                              boolean autoHwmEnabled,
                              AutoHwmProfile autoHwmProfile,
                              int autoHwmRole, int autoHwmPolicyClass,
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
                              long oversizeMessageAdmissionMaxBytes,
                              long flowPausedConnections,
                              long flowPauseAppliedTotal,
                              long flowResumeAppliedTotal,
                              long flowStateStaleTotal,
                              long flowPauseDurationMs) {
    public boolean isReady() {
        return stateFlags.contains(MonitorStateFlags.READY);
    }
}
