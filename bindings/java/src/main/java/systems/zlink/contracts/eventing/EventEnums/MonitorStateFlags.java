/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import java.util.EnumSet;

/** State bits reported by a monitor status snapshot. */
public enum MonitorStateFlags {
    /** The monitored source is ready for traffic. */
    READY(1 << 0),
    /** The monitored source has a ready bound endpoint. */
    BOUND_READY(1 << 1),
    /** The monitored source is closed. */
    CLOSED(1 << 3);

    private final int mask;

    MonitorStateFlags(int mask) {
        this.mask = mask;
    }

    public int mask() {
        return mask;
    }

    private static final MonitorStateFlags[] VALUES = values();

    public static EnumSet<MonitorStateFlags> fromMask(int mask) {
        EnumSet<MonitorStateFlags> out = EnumSet.noneOf(MonitorStateFlags.class);
        for (MonitorStateFlags flag : VALUES) {
            if ((mask & flag.mask) != 0) {
                out.add(flag);
            }
        }
        return out;
    }
}
