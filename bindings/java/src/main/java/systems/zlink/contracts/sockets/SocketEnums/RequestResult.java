/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** The outcome of a request. */
public enum RequestResult {
    OK(0),
    TIMED_OUT(101),
    NOT_FOUND(102),
    TERMINATED(103),
    PROTOCOL_ERROR(104),
    INTERNAL_ERROR(105),
    REJECTED(106),
    CONFLICT(107),
    BUSY(108),
    NOT_CONNECTED(109),
    INVALID_ARGUMENT(110),
    INVALID_STATE(111),
    NOT_SUPPORTED(112),
    BACKPRESSURED(113);

    private final int value;

    RequestResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    private static final RequestResult[] VALUES = values();

    public static RequestResult fromValue(int value) {
        for (RequestResult result : VALUES) {
            if (result.value == value) {
                return result;
            }
        }
        throw new IllegalArgumentException("invalid RequestResult value: " + value);
    }
}
