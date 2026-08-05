/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Determines how a socket reacts to a peer that reuses an existing routing id. */
public enum RidDuplicatePolicy {
    /** Reject the new peer and keep the existing route. */
    REJECT(0),
    /** Hand the routing id to the new peer, dropping the previous holder. */
    HANDOVER(1);

    private final int value;

    RidDuplicatePolicy(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static RidDuplicatePolicy fromValue(int value) {
        return switch (value) {
            case 0 -> REJECT;
            case 1 -> HANDOVER;
            default -> throw new IllegalArgumentException(
                "Invalid RidDuplicatePolicy value: " + value);
        };
    }
}
