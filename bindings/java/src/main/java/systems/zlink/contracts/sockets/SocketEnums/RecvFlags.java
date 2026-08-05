/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** Flags that modify receive behavior. */
public enum RecvFlags {
    /** Default receive behavior: block until a message is available. */
    NONE(0),
    /** Do not block; return immediately when no message is available. */
    DONT_WAIT(1);

    private final int value;

    RecvFlags(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static RecvFlags fromValue(int value) {
        return switch (value) {
            case 0 -> NONE;
            case 1 -> DONT_WAIT;
            default -> throw new IllegalArgumentException(
                "invalid RecvFlags value: " + value);
        };
    }
}
