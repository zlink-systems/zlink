/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/**
 * Selects an automatic high-water-mark sizing profile that trades memory,
 * latency, and throughput.
 */
public enum AutoHwmProfile {
    /** Smallest queues, minimizing memory use. */
    COMPACT(0),
    /** Small queues that drain quickly to favor latency. */
    LOW_LATENCY(1),
    /** Balances latency against throughput. */
    BALANCED(2),
    /** Large queues that favor throughput. */
    THROUGHPUT(3);

    private final int value;

    AutoHwmProfile(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static AutoHwmProfile fromValue(int value) {
        return switch (value) {
            case 0 -> COMPACT;
            case 1 -> LOW_LATENCY;
            case 2 -> BALANCED;
            case 3 -> THROUGHPUT;
            default -> throw new IllegalArgumentException(
                "Invalid AutoHwmProfile value: " + value);
        };
    }
}
