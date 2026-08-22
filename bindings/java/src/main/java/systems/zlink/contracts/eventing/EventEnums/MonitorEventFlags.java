/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import java.util.EnumSet;

/**
 * Event-specific flags carried in {@link MonitorEvent#flags()}. Mirrors
 * {@code zlink_monitor_event_flag_e} in
 * core/include/zlink/eventing/api.h. Combine as flags.
 */
public enum MonitorEventFlags {
    /**
     * Set on a {@link MonitorEventType#CONNECTION_READY} event that moves a
     * connection from not-ready to ready.
     */
    CONNECTION_READY_EDGE(1 << 0),
    /**
     * Set on {@link MonitorEventType#SEND_FLOW_RESUMED} when clearing the
     * remote pause left the pipe actually writable. Clear when another
     * cause (byte high-water-mark, transport wait, or termination) still
     * blocks it.
     */
    SEND_FLOW_WRITABLE(1 << 1),
    /**
     * Set on {@link MonitorEventType#FLOW_STATE_STALE} when the frame named
     * a different connection generation.
     */
    FLOW_STATE_STALE_GENERATION(1 << 2),
    /**
     * Set on {@link MonitorEventType#FLOW_STATE_STALE} when the epoch did
     * not advance inside the current generation.
     */
    FLOW_STATE_STALE_EPOCH(1 << 3);

    private final int mask;

    MonitorEventFlags(int mask) {
        this.mask = mask;
    }

    public int mask() {
        return mask;
    }

    private static final MonitorEventFlags[] VALUES = values();

    public static EnumSet<MonitorEventFlags> fromMask(int mask) {
        EnumSet<MonitorEventFlags> out = EnumSet.noneOf(MonitorEventFlags.class);
        for (MonitorEventFlags flag : VALUES) {
            if ((mask & flag.mask) != 0) {
                out.add(flag);
            }
        }
        return out;
    }
}
