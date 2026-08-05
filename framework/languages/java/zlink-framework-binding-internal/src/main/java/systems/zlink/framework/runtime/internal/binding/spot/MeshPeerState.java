/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Connection state of a mesh peer. */
public enum MeshPeerState {
    CONFIGURED(1),
    CONNECTING(2),
    ADMITTED(3),
    DRAINING(4),
    CLOSED(5),
    ERROR(6),
    NOT_REQUIRED(7);

    private final int value;

    MeshPeerState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static MeshPeerState fromValue(int value) {
        for (MeshPeerState state : values()) {
            if (state.value == value) {
                return state;
            }
        }
        throw new IllegalArgumentException("invalid MeshPeerState value: " + value);
    }
}
