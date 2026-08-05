/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Identifies a socket's messaging pattern. */
public enum SocketType {
    /** Matches any socket type; used as a filter value, not a real type. */
    ANY(0),
    PAIR(0x1001),
    PUB(0x1002),
    SUB(0x1003),
    DEALER(0x1004),
    ROUTER(0x1005),
    XPUB(0x1006),
    XSUB(0x1007),
    STREAM(0x1008);

    private final int value;

    SocketType(int value) {
        this.value = value;
    }

    public int getValue() { return value; }

    public static SocketType fromValue(int value) {
        return switch (value) {
            case 0 -> ANY;
            case 0x1001 -> PAIR;
            case 0x1002 -> PUB;
            case 0x1003 -> SUB;
            case 0x1004 -> DEALER;
            case 0x1005 -> ROUTER;
            case 0x1006 -> XPUB;
            case 0x1007 -> XSUB;
            case 0x1008 -> STREAM;
            default -> throw new IllegalArgumentException(
                "Invalid SocketType value: " + value);
        };
    }
}
