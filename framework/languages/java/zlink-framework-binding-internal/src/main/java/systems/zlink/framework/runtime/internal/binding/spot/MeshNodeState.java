/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Lifecycle state of a mesh node. */
public enum MeshNodeState {
    CREATED(1),
    STARTED(2),
    PARTIAL_READY(3),
    READY(4),
    DRAINING(5),
    STOPPED(6),
    ERROR(7);

    private final int value;

    MeshNodeState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static MeshNodeState fromValue(int value) {
        for (MeshNodeState state : values()) {
            if (state.value == value) {
                return state;
            }
        }
        throw new IllegalArgumentException("invalid MeshNodeState value: " + value);
    }
}
