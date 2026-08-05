/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** Flags that modify send behavior. */
public enum SendFlags {
    /** Default send behavior: block until the message can be queued. */
    NONE(0),
    /** Do not block; report back-pressure instead of waiting. */
    DONT_WAIT(1);

    private final int value;

    SendFlags(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static SendFlags fromValue(int value) {
        return switch (value) {
            case 0 -> NONE;
            case 1 -> DONT_WAIT;
            default -> throw new IllegalArgumentException(
                "invalid SendFlags value: " + value);
        };
    }
}
