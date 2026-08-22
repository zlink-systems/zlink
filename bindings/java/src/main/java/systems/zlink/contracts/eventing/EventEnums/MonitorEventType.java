/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

/**
 * The kind of a delivered socket monitor event; mirrors the connection
 * lifecycle events that a monitor can be subscribed to.
 */
public enum MonitorEventType {
    /** A connection to a peer was established. */
    CONNECTED(0x0001),
    /** An asynchronous connect is still in progress. */
    CONNECT_DELAYED(0x0002),
    /** A failed connect will be retried after a delay. */
    CONNECT_RETRIED(0x0004),
    /** The socket began listening on a bound endpoint. */
    LISTENING(0x0008),
    /** Binding to an endpoint failed. */
    BIND_FAILED(0x0010),
    /** An inbound connection was accepted. */
    ACCEPTED(0x0020),
    /** Accepting an inbound connection failed. */
    ACCEPT_FAILED(0x0040),
    /** A connection was closed. */
    CLOSED(0x0080),
    /** Closing a connection failed. */
    CLOSE_FAILED(0x0100),
    /** A peer disconnected. */
    DISCONNECTED(0x0200),
    /** Monitoring of the socket has stopped. */
    MONITOR_STOPPED(0x0400),
    /** The connection handshake failed without further detail. */
    HANDSHAKE_FAILED_NO_DETAIL(0x0800),
    /** The connection completed its handshake and is ready for traffic. */
    CONNECTION_READY(0x1000),
    /** The handshake failed due to a protocol error. */
    HANDSHAKE_FAILED_PROTOCOL(0x2000),
    /** The handshake failed authentication. */
    HANDSHAKE_FAILED_AUTH(0x4000),
    /** A peer's load-balancing weight changed. */
    PEER_WEIGHT_CHANGED(0x8000),
    /**
     * A paired DEALER/ROUTER completion lane applied a remote PAUSED
     * transition to an application pipe. No event fires for a normal data
     * frame.
     */
    SEND_FLOW_PAUSED(0x10000),
    /**
     * A paired DEALER/ROUTER completion lane applied a remote RUNNING
     * transition, resuming an application pipe.
     */
    SEND_FLOW_RESUMED(0x20000),
    /** A stale or duplicate receive-flow-state frame was ignored. */
    FLOW_STATE_STALE(0x40000),
    /** Every event; subscribes the monitor to all of the above. */
    ALL(0x7FFFF);

    private final int value;

    MonitorEventType(int value) {
        this.value = value;
    }

    public int getValue() { return value; }

    private static final MonitorEventType[] VALUES = values();

    public static MonitorEventType fromValue(long value) {
        for (MonitorEventType type : VALUES) {
            if (type.value == (int) value) {
                return type;
            }
        }
        throw new IllegalArgumentException("Invalid MonitorEventType value: "
            + value);
    }

    public static int combine(MonitorEventType... flags) {
        int mask = 0;
        for (MonitorEventType flag : flags) {
            mask |= flag.value;
        }
        return mask;
    }
}
