/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The kind of a pending mesh operation. */
public enum OperationKind {
    NONE(0),
    NODE_REQUEST(1),
    CHANNEL_REQUEST(2),
    SPOT_REQUEST(3),
    ACTOR_REQUEST(4),
    ACTOR_LOOKUP(5),
    ACTOR_DESTROY(6),
    ACTOR_JOIN(7),
    ACTOR_LEAVE(8),
    STREAM_BIND(9),
    STREAM_UNBIND(10),
    STREAM_CLOSE(11);

    private final int value;

    OperationKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static OperationKind fromValue(int value) {
        for (OperationKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("invalid OperationKind value: " + value);
    }
}
