/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

/** Low-level send flags used by the native socket API. */
enum SendFlag {
    NONE(0), DONTWAIT(1), SNDMORE(2), DONTWAIT_SNDMORE(3);

    private final int value;
    SendFlag(int v) { this.value = v; }
    public int getValue() { return value; }

    public static SendFlag combine(SendFlag first, SendFlag second) {
        if (first == null || second == null)
            throw new NullPointerException("flags");
        return fromValue(first.value | second.value);
    }

    public static SendFlag combine(SendFlag... flags) {
        int v = 0;
        for (var f : flags) v |= f.value;
        return fromValue(v);
    }

    public static SendFlag fromValue(int value) {
        return switch (value) {
            case 0 -> NONE;
            case 1 -> DONTWAIT;
            case 2 -> SNDMORE;
            case 3 -> DONTWAIT_SNDMORE;
            default -> throw new IllegalArgumentException(
              "invalid SendFlag value: " + value);
        };
    }
}
