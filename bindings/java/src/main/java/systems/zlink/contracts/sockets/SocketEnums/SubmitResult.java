/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** The outcome of submitting a send or publish. */
public enum SubmitResult {
    OK(0),
    BACKPRESSURED(1),
    NOT_CONNECTED(2),
    NOT_FOUND(3),
    TERMINATED(4),
    INVALID_HANDLE(5),
    INVALID_ARGUMENT(6),
    NOT_SUPPORTED(7),
    INVALID_STATE(8),
    THREAD_VIOLATION(9),
    OUT_OF_MEMORY(10),
    SEQ_EXHAUSTED(11),
    INTERNAL_ERROR(12),
    NOT_ADMITTED(13);

    private final int value;

    SubmitResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    private static final SubmitResult[] VALUES = values();

    public static SubmitResult fromValue(int value) {
        for (SubmitResult result : VALUES) {
            if (result.value == value) {
                return result;
            }
        }
        throw new IllegalArgumentException("invalid SubmitResult value: " + value);
    }
}
