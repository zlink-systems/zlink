/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** A phase reported by the Core actor transfer fence. */
enum ActorTransferPhase {
    PREPARING(1),
    FENCED(2),
    COMMITTED(3),
    ACTIVATED(4),
    ABORTED(5);

    private final int value;

    ActorTransferPhase(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ActorTransferPhase fromValue(int value) {
        for (ActorTransferPhase phase : values()) {
            if (phase.value == value) {
                return phase;
            }
        }
        throw new IllegalArgumentException("invalid ActorTransferPhase value: " + value);
    }
}
