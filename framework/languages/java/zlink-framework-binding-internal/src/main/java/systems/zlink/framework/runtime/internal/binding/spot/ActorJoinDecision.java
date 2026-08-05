/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Result of an actor join reply. */
public enum ActorJoinDecision {
    ACCEPTED(0),
    REJECTED(1);

    private final int value;

    ActorJoinDecision(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ActorJoinDecision fromValue(int value) {
        for (ActorJoinDecision decision : values()) {
            if (decision.value == value) {
                return decision;
            }
        }
        throw new IllegalArgumentException("invalid ActorJoinDecision value: " + value);
    }
}
