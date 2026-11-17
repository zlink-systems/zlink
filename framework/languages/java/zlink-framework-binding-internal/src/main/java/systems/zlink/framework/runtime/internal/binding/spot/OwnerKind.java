/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The owner kind of a dispatch ready record. */
public enum OwnerKind {
    NODE(1),
    SPOT(2),
    ACTOR(3);

    private final int value;

    OwnerKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static OwnerKind fromValue(int value) {
        for (OwnerKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("invalid OwnerKind value: " + value);
    }
}
