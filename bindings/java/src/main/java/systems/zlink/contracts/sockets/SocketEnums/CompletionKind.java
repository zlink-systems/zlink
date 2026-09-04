/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Identifies the kind of a native socket completion record. */
public enum CompletionKind {
    /**
     * ABI-retained legacy SEND value. Ordinary successful sends do not emit a
     * SEND completion.
     */
    SEND(1),
    /** A request reached its asynchronous terminal result. */
    REQUEST(2),
    /**
     * Resolves a previously backpressured send token. A writable result allows
     * the same packet to be retried; terminal fields report shutdown instead.
     */
    WRITABLE(3);

    private final int value;

    CompletionKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static CompletionKind fromValue(int value) {
        return switch (value) {
            case 1 -> SEND;
            case 2 -> REQUEST;
            case 3 -> WRITABLE;
            default -> throw new IllegalArgumentException(
                "invalid CompletionKind value: " + value);
        };
    }
}
