/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The destination family whose send capacity became available. */
public enum MeshDestinationKind {
    NODE(1),
    CHANNEL(2),
    SPOT(3),
    ACTOR(4),
    BOUND_SESSION(5);

    private final int value;

    MeshDestinationKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static MeshDestinationKind fromValue(int value) {
        for (MeshDestinationKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException(
            "invalid MeshDestinationKind value: " + value);
    }
}
