/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Selects raw record or framed packet STREAM receive semantics. */
public enum StreamRecvMode {
    UNSPECIFIED(0),
    RAW(1),
    PACKET(2);

    private final int value;

    StreamRecvMode(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static StreamRecvMode fromValue(int value) {
        return switch (value) {
            case 0 -> UNSPECIFIED;
            case 1 -> RAW;
            case 2 -> PACKET;
            default -> throw new IllegalArgumentException(
                "unknown StreamRecvMode value: " + value);
        };
    }
}
