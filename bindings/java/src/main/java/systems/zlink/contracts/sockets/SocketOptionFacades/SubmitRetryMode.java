/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Determines whether an eligible blocking local failure is retried by Core. */
public enum SubmitRetryMode {
    /** Never retry; a failed submit fails immediately. */
    OFF(0),
    /**
     * Retry eligible blocking local connection failures. This does not turn
     * DONTWAIT backpressure into a Core-retained pending SEND.
     */
    LOCAL_FAILURE(1);

    private final int value;

    SubmitRetryMode(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static SubmitRetryMode fromValue(int value) {
        return switch (value) {
            case 0 -> OFF;
            case 1 -> LOCAL_FAILURE;
            default -> throw new IllegalArgumentException(
                "Invalid SubmitRetryMode value: " + value);
        };
    }
}
