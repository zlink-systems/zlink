/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import java.util.EnumSet;

/** Detail bits describing which monitor status fields are populated. */
public enum MonitorStatusDetailFlags {
    /** Send pending message telemetry is populated. */
    SEND_PENDING_MESSAGES(1 << 1),
    /** Receive pending message telemetry is populated. */
    RECEIVE_PENDING_MESSAGES(1 << 2),
    /** Automatic high-water-mark budget telemetry is populated. */
    AUTO_HWM_BUDGET(1 << 3),
    /** Automatic high-water-mark buffer telemetry is populated. */
    AUTO_HWM_BUFFERS(1 << 4);

    private final int mask;

    MonitorStatusDetailFlags(int mask) {
        this.mask = mask;
    }

    public int mask() {
        return mask;
    }

    private static final MonitorStatusDetailFlags[] VALUES = values();

    public static EnumSet<MonitorStatusDetailFlags> fromMask(int mask) {
        EnumSet<MonitorStatusDetailFlags> out =
            EnumSet.noneOf(MonitorStatusDetailFlags.class);
        for (MonitorStatusDetailFlags flag : VALUES) {
            if ((mask & flag.mask) != 0) {
                out.add(flag);
            }
        }
        return out;
    }
}
