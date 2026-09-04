/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import java.util.EnumSet;

/** Readiness conditions a poll source can be watched for or report. Combine as flags. */
public enum PollEventFlags {
    /** Readable: a receive will not block. */
    POLLIN(1),
    /**
     * Writable: a send will not block. An unread WRITABLE completion from a
     * previously backpressured DONTWAIT send also keeps this flag ready.
     */
    POLLOUT(2),
    /** An error condition occurred on the source. */
    POLLERR(4),
    /** Priority or out-of-band data is available. */
    POLLPRI(8),
    /**
     * A completion record is available. Ordinary successful sends do not
     * produce one; WRITABLE send recovery is signaled through POLLOUT.
     */
    POLLCOMPLETION(32);

    private final int mask;

    PollEventFlags(int mask) {
        this.mask = mask;
    }

    public int mask() {
        return mask;
    }

    int value() {
        return mask();
    }

    static int combine(PollEventFlags... flags) {
        int out = 0;
        for (PollEventFlags flag : flags) {
            out |= flag.mask;
        }
        return out;
    }

    private static final PollEventFlags[] VALUES = values();

    static EnumSet<PollEventFlags> fromMask(int mask) {
        EnumSet<PollEventFlags> out = EnumSet.noneOf(PollEventFlags.class);
        for (PollEventFlags flag : VALUES) {
            if ((mask & flag.mask) != 0) {
                out.add(flag);
            }
        }
        return out;
    }
}
