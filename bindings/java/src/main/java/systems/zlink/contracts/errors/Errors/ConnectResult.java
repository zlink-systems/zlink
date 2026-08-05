/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;


/** The outcome of connecting a socket to an endpoint. */
public enum ConnectResult {
    OK(0),
    INVALID_ARGUMENT(601),
    NOT_SUPPORTED(602),
    INVALID_HANDLE(603),
    INTERNAL_ERROR(604),
    NOT_FOUND(605),
    CONFLICT(606),
    BUSY(607);

    private final int value;

    ConnectResult(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    private static final ConnectResult[] VALUES = values();

    public static ConnectResult fromValue(int value) {
        for (ConnectResult result : VALUES) {
            if (result.value == value) {
                return result;
            }
        }
        throw new IllegalArgumentException("invalid ConnectResult value: " + value);
    }
}
