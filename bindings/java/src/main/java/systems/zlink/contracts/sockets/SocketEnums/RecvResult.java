/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** The outcome of a receive. */
public enum RecvResult {
    OK(0),
    NO_DATA(201),
    BUSY(202),
    TERMINATED(203),
    INVALID_HANDLE(204),
    NOT_SUPPORTED(205),
    INTERNAL_ERROR(206),
    BUFFER_TOO_SMALL(207),
    INVALID_STATE(208);

    private final int value;

    RecvResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static RecvResult fromValue(int value) {
        return switch (value) {
            case 0 -> OK;
            case 201 -> NO_DATA;
            case 202 -> BUSY;
            case 203 -> TERMINATED;
            case 204 -> INVALID_HANDLE;
            case 205 -> NOT_SUPPORTED;
            case 206 -> INTERNAL_ERROR;
            case 207 -> BUFFER_TOO_SMALL;
            case 208 -> INVALID_STATE;
            default -> throw new IllegalArgumentException(
                "invalid RecvResult value: " + value);
        };
    }
}
