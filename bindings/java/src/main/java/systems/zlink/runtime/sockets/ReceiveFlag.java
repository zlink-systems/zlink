/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

/** Low-level receive flags used by the native socket API. */
enum ReceiveFlag {
    NONE(0), DONTWAIT(1);

    private final int value;
    ReceiveFlag(int v) { this.value = v; }
    public int getValue() { return value; }

    public static ReceiveFlag fromValue(int value) {
        return switch (value) {
            case 0 -> NONE;
            case 1 -> DONTWAIT;
            default -> throw new IllegalArgumentException(
              "invalid ReceiveFlag value: " + value);
        };
    }

    public static int combine(ReceiveFlag... flags) {
        int v = 0;
        for (var f : flags) v |= f.value;
        return v;
    }
}
