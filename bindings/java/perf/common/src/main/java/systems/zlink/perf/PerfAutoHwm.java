/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.sockets.AutoHwmRecalcReason;
import systems.zlink.contracts.eventing.MonitorStatus;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SocketType;
import java.util.Locale;

final class PerfAutoHwm {
    private PerfAutoHwm() {
    }

    static void printSingleMonitor(PerfUtil.Config config,
                                   SocketMonitor monitor, String component,
                                   SocketType socketType) {
        MonitorStatus monitorStatus = monitor.status();
        if (!visible(monitorStatus)) {
            return;
        }
        System.out.println("AUTO_HWM_DETAIL"
            + ",pattern=" + config.pattern()
            + ",transport=" + config.transport()
            + ",component=" + component
            + ",msg_size=" + config.size()
            + ",owner=socket"
            + ",owner_id=0"
            + ",socket=" + component
            + ",socket_type=" + socketTypeName(socketType)
            + ",role=" + roleName(monitorStatus.autoHwmRole())
            + ",sndhwm=" + monitorStatus.autoHwmAppliedSendHwmBytes()
            + ",rcvhwm=" + monitorStatus.autoHwmAppliedRecvHwmBytes()
            + ",effective_message_bytes="
            + monitorStatus.autoHwmEffectiveMessageBytes()
            + ",effective_sndbuf=" + monitorStatus.autoHwmAppliedSndBuffer()
            + ",effective_rcvbuf=" + monitorStatus.autoHwmAppliedRcvBuffer()
            + ",socket_message_slots="
            + monitorStatus.autoHwmSocketMessageSlots());
    }

    static void printMultiSocket(PerfUtil.Config config, Socket socket,
                                 String component, String label,
                                 SocketType socketType) {
        try (SocketMonitor monitor = socket.monitorOpen()) {
            printMultiMonitor(config, monitor, component, label, socketType);
        }
    }

    static void printMultiMonitor(PerfUtil.Config config, SocketMonitor monitor,
                                  String component, String label,
                                  SocketType socketType) {
        MonitorStatus monitorStatus = monitor.status();
        if (!visible(monitorStatus)) {
            return;
        }
        System.out.println("AUTO_HWM_DETAIL"
            + ",pattern=MULTI_" + config.pattern()
            + ",transport=" + config.transport()
            + ",component=" + component
            + ",label=" + label
            + ",socket_type=" + socketTypeName(socketType)
            + ",msg_size=" + config.size()
            + ",source=monitor_snapshot"
            + ",enabled=" + (monitorStatus.autoHwmEnabled() ? 1 : 0)
            + ",role=" + roleName(monitorStatus.autoHwmRole())
            + ",role_id=" + monitorStatus.autoHwmRole()
            + ",profile=" + profileName(monitorStatus.autoHwmProfile())
            + ",profile_id=" + profileId(monitorStatus.autoHwmProfile())
            + ",policy_class=" + policyClassName(monitorStatus.autoHwmPolicyClass())
            + ",policy_class_id=" + monitorStatus.autoHwmPolicyClass()
            + ",unit_budget_bytes=" + monitorStatus.autoHwmUnitBudgetBytes()
            + ",size_cap=" + monitorStatus.autoHwmSizeCap()
            + ",sndhwm=" + hwmDisplay(monitorStatus, socketType, true)
            + ",rcvhwm=" + hwmDisplay(monitorStatus, socketType, false)
            + ",socket_message_slots=" + monitorStatus.autoHwmSocketMessageSlots()
            + ",effective_message_bytes="
            + monitorStatus.autoHwmEffectiveMessageBytes()
            + ",effective_sndbuf=" + bufferDisplay(monitorStatus, socketType, true)
            + ",effective_rcvbuf=" + bufferDisplay(monitorStatus, socketType, false)
            + ",last_recalc_ms=" + monitorStatus.autoHwmLastRecalcMs()
            + ",last_recalc_reason="
            + recalcReasonName(monitorStatus.autoHwmLastRecalcReason())
            + ",send_blocked_ratio_ppm="
            + monitorStatus.autoHwmSendBlockedRatioPpm()
            + ",deferred_sndhwm="
            + monitorStatus.autoHwmDeferredSendHwmBytes()
            + ",deferred_rcvhwm="
            + monitorStatus.autoHwmDeferredRecvHwmBytes());
    }

    private static boolean visible(MonitorStatus monitorStatus) {
        return monitorStatus.autoHwmAppliedSendHwmBytes() > 0
            || monitorStatus.autoHwmAppliedRecvHwmBytes() > 0
            || monitorStatus.autoHwmEffectiveMessageBytes() > 0
            || monitorStatus.autoHwmSocketMessageSlots() > 0;
    }

    private static String roleName(int role) {
        return switch (role) {
            case 1 -> "control";
            case 2 -> "routed";
            case 3 -> "fanout";
            case 4 -> "recv_ingress";
            case 5 -> "spot_data";
            case 6 -> "peer_queue";
            case 7 -> "stream";
            default -> "none";
        };
    }

    private static String policyClassName(int policyClass) {
        return switch (policyClass) {
            case 1 -> "fanout";
            case 2 -> "spot_data";
            case 3 -> "recv_ingress";
            case 4 -> "routed";
            case 5 -> "peer_queue";
            case 6 -> "stream";
            case 7 -> "control";
            default -> "none";
        };
    }

    private static String profileName(systems.zlink.contracts.sockets.AutoHwmProfile profile) {
        return switch (profile) {
            case COMPACT -> "compact";
            case LOW_LATENCY -> "low_latency";
            case BALANCED -> "balanced";
            case THROUGHPUT -> "throughput";
        };
    }

    private static int profileId(systems.zlink.contracts.sockets.AutoHwmProfile profile) {
        return switch (profile) {
            case COMPACT -> 0;
            case LOW_LATENCY -> 1;
            case BALANCED -> 2;
            case THROUGHPUT -> 3;
        };
    }

    private static String recalcReasonName(
      systems.zlink.contracts.sockets.AutoHwmRecalcReason reason) {
        return switch (reason) {
            case INITIAL -> "initial";
            case ROLE_CHANGE -> "role_change";
            case POLICY_TOGGLE -> "policy_toggle";
            case REFRESH -> "refresh";
            case DEFERRED_SHRINK -> "deferred_shrink";
            default -> "none";
        };
    }

    private static String socketTypeName(SocketType type) {
        return type.name().toLowerCase(Locale.ROOT);
    }

    private static String hwmDisplay(MonitorStatus monitorStatus,
                                     SocketType socketType,
                                     boolean sendSide) {
        if (!sideVisible(socketType, monitorStatus.autoHwmRole(), sendSide)) {
            return "-";
        }
        return Long.toString(sendSide
            ? monitorStatus.autoHwmAppliedSendHwmBytes()
            : monitorStatus.autoHwmAppliedRecvHwmBytes());
    }

    private static String bufferDisplay(MonitorStatus monitorStatus,
                                        SocketType socketType,
                                        boolean sendSide) {
        if (!sideVisible(socketType, monitorStatus.autoHwmRole(), sendSide)) {
            return "0";
        }
        return Integer.toString(sendSide
            ? monitorStatus.autoHwmAppliedSndBuffer()
            : monitorStatus.autoHwmAppliedRcvBuffer());
    }

    private static boolean sideVisible(SocketType socketType, int role,
                                       boolean sendSide) {
        String roleName = roleName(role);
        if (sendSide
            && (socketType == SocketType.SUB || socketType == SocketType.XSUB)
            && ("recv_ingress".equals(roleName) || "control".equals(roleName))) {
            return false;
        }
        return sendSide
            || !((socketType == SocketType.PUB || socketType == SocketType.XPUB)
            && ("spot_data".equals(roleName) || "control".equals(roleName)));
    }
}
