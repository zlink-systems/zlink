/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.sockets.AutoHwmRecalcReason;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.MonitorSourceKind;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.RidDuplicatePolicy;
import systems.zlink.contracts.sockets.SocketType;
import java.util.EnumSet;

/**
 * Non-exported numeric enum codec table shared by binding internals.
 */
public final class EnumCodecs {
    private EnumCodecs() {
    }

    public static int autoHwmProfileValue(AutoHwmProfile value) {
        return switch (value) {
            case COMPACT -> 0;
            case LOW_LATENCY -> 1;
            case BALANCED -> 2;
            case THROUGHPUT -> 3;
        };
    }

    public static AutoHwmProfile autoHwmProfileFromValue(int value) {
        return switch (value) {
            case 0 -> AutoHwmProfile.COMPACT;
            case 1 -> AutoHwmProfile.LOW_LATENCY;
            case 2 -> AutoHwmProfile.BALANCED;
            case 3 -> AutoHwmProfile.THROUGHPUT;
            default -> throw invalid("AutoHwmProfile", value);
        };
    }

    public static AutoHwmRecalcReason autoHwmRecalcReasonFromValue(int value) {
        return switch (value) {
            case 0 -> AutoHwmRecalcReason.NONE;
            case 1 -> AutoHwmRecalcReason.INITIAL;
            case 2 -> AutoHwmRecalcReason.ROLE_CHANGE;
            case 3 -> AutoHwmRecalcReason.POLICY_TOGGLE;
            case 4 -> AutoHwmRecalcReason.REFRESH;
            case 5 -> AutoHwmRecalcReason.DEFERRED_SHRINK;
            default -> throw invalid("AutoHwmRecalcReason", value);
        };
    }

    public static int autoHwmRecalcReasonValue(AutoHwmRecalcReason value) {
        return switch (value) {
            case NONE -> 0;
            case INITIAL -> 1;
            case ROLE_CHANGE -> 2;
            case POLICY_TOGGLE -> 3;
            case REFRESH -> 4;
            case DEFERRED_SHRINK -> 5;
        };
    }

    public static MonitorEventType monitorEventTypeFromValue(long value) {
        return switch ((int) value) {
            case 0x0001 -> MonitorEventType.CONNECTED;
            case 0x0002 -> MonitorEventType.CONNECT_DELAYED;
            case 0x0004 -> MonitorEventType.CONNECT_RETRIED;
            case 0x0008 -> MonitorEventType.LISTENING;
            case 0x0010 -> MonitorEventType.BIND_FAILED;
            case 0x0020 -> MonitorEventType.ACCEPTED;
            case 0x0040 -> MonitorEventType.ACCEPT_FAILED;
            case 0x0080 -> MonitorEventType.CLOSED;
            case 0x0100 -> MonitorEventType.CLOSE_FAILED;
            case 0x0200 -> MonitorEventType.DISCONNECTED;
            case 0x0400 -> MonitorEventType.MONITOR_STOPPED;
            case 0x0800 -> MonitorEventType.HANDSHAKE_FAILED_NO_DETAIL;
            case 0x1000 -> MonitorEventType.CONNECTION_READY;
            case 0x2000 -> MonitorEventType.HANDSHAKE_FAILED_PROTOCOL;
            case 0x4000 -> MonitorEventType.HANDSHAKE_FAILED_AUTH;
            case 0x8000 -> MonitorEventType.PEER_WEIGHT_CHANGED;
            case 0xFFFF -> MonitorEventType.ALL;
            default -> throw invalid("MonitorEventType", value);
        };
    }

    public static int monitorEventTypeValue(MonitorEventType value) {
        return switch (value) {
            case CONNECTED -> 0x0001;
            case CONNECT_DELAYED -> 0x0002;
            case CONNECT_RETRIED -> 0x0004;
            case LISTENING -> 0x0008;
            case BIND_FAILED -> 0x0010;
            case ACCEPTED -> 0x0020;
            case ACCEPT_FAILED -> 0x0040;
            case CLOSED -> 0x0080;
            case CLOSE_FAILED -> 0x0100;
            case DISCONNECTED -> 0x0200;
            case MONITOR_STOPPED -> 0x0400;
            case HANDSHAKE_FAILED_NO_DETAIL -> 0x0800;
            case CONNECTION_READY -> 0x1000;
            case HANDSHAKE_FAILED_PROTOCOL -> 0x2000;
            case HANDSHAKE_FAILED_AUTH -> 0x4000;
            case PEER_WEIGHT_CHANGED -> 0x8000;
            case ALL -> 0xFFFF;
        };
    }

    public static int monitorEventMask(MonitorEventType... values) {
        int mask = 0;
        for (MonitorEventType value : values) {
            mask |= monitorEventTypeValue(value);
        }
        return mask;
    }

    public static MonitorSourceKind monitorSourceKindFromValue(int value) {
        return switch (value) {
            case 1 -> MonitorSourceKind.SOCKET;
            default -> throw invalid("MonitorSourceKind", value);
        };
    }

    public static int monitorSourceKindValue(MonitorSourceKind value) {
        return 1;
    }

    public static int pollEventFlagValue(PollEventFlags value) {
        return switch (value) {
            case POLLIN -> 1;
            case POLLOUT -> 2;
            case POLLERR -> 4;
            case POLLPRI -> 8;
            case POLLCOMPLETION -> 32;
        };
    }

    public static int pollEventMask(PollEventFlags... values) {
        int mask = 0;
        for (PollEventFlags value : values) {
            mask |= pollEventFlagValue(value);
        }
        return mask;
    }

    public static EnumSet<PollEventFlags> pollEventFlagsFromMask(int mask) {
        EnumSet<PollEventFlags> out = EnumSet.noneOf(PollEventFlags.class);
        for (PollEventFlags value : PollEventFlags.values()) {
            if ((mask & pollEventFlagValue(value)) != 0) {
                out.add(value);
            }
        }
        return out;
    }

    public static int ridDuplicatePolicyValue(RidDuplicatePolicy value) {
        return switch (value) {
            case REJECT -> 0;
            case HANDOVER -> 1;
        };
    }

    public static RidDuplicatePolicy ridDuplicatePolicyFromValue(int value) {
        return switch (value) {
            case 0 -> RidDuplicatePolicy.REJECT;
            case 1 -> RidDuplicatePolicy.HANDOVER;
            default -> throw invalid("RidDuplicatePolicy", value);
        };
    }

    public static int socketTypeValue(SocketType value) {
        return switch (value) {
            case ANY -> 0;
            case PAIR -> 0x1001;
            case PUB -> 0x1002;
            case SUB -> 0x1003;
            case DEALER -> 0x1004;
            case ROUTER -> 0x1005;
            case XPUB -> 0x1006;
            case XSUB -> 0x1007;
            case STREAM -> 0x1008;
        };
    }

    public static SocketType socketTypeFromValue(int value) {
        return switch (value) {
            case 0 -> SocketType.ANY;
            case 0x1001 -> SocketType.PAIR;
            case 0x1002 -> SocketType.PUB;
            case 0x1003 -> SocketType.SUB;
            case 0x1004 -> SocketType.DEALER;
            case 0x1005 -> SocketType.ROUTER;
            case 0x1006 -> SocketType.XPUB;
            case 0x1007 -> SocketType.XSUB;
            case 0x1008 -> SocketType.STREAM;
            default -> throw invalid("SocketType", value);
        };
    }

    private static IllegalArgumentException invalid(String type, long value) {
        return new IllegalArgumentException(
            "invalid " + type + " value: " + value);
    }
}
