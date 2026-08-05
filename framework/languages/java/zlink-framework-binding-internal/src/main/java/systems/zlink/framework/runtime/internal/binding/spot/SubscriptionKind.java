/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Matching kind for a spot topic subscription. */
public enum SubscriptionKind {
    EXACT(1),
    PREFIX(2);

    private final int value;

    SubscriptionKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static SubscriptionKind fromValue(int value) {
        for (SubscriptionKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("invalid SubscriptionKind value: " + value);
    }
}
