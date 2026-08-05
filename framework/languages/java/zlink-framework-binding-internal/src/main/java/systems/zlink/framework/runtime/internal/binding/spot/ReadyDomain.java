/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Dispatch ready-index domains. Values are bit masks. */
public enum ReadyDomain {
    NONE(0),
    APPLICATION(1),
    INFRASTRUCTURE(2),
    ALL(3);

    private final int value;

    ReadyDomain(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ReadyDomain fromValue(int value) {
        for (ReadyDomain domain : values()) {
            if (domain.value == value) {
                return domain;
            }
        }
        throw new IllegalArgumentException("invalid ReadyDomain value: " + value);
    }

    /** Combines the given domains into a single bit mask. */
    public static int mask(ReadyDomain... domains) {
        int mask = 0;
        for (ReadyDomain domain : domains) {
            mask |= domain.value;
        }
        return mask;
    }
}
