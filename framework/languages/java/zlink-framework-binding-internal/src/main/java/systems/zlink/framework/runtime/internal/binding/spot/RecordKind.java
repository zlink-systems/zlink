/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The kind of a received dispatch record. */
public enum RecordKind {
    NODE_SEND(1),
    NODE_REQUEST(2),
    CHANNEL_SEND(3),
    CHANNEL_REQUEST(4),
    SPOT_SEND(5),
    SPOT_REQUEST(6),
    SPOT_MULTICAST(7),
    SPOT_CONTROL(8),
    ACTOR_SEND(9),
    ACTOR_REQUEST(10),
    COMPLETION(11),
    SEND_READY(12),
    TRANSFER_CONTROL(13);

    private final int value;

    RecordKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static RecordKind fromValue(int value) {
        for (RecordKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("invalid RecordKind value: " + value);
    }
}
