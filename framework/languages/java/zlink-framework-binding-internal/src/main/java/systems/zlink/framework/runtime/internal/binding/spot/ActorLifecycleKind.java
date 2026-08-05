/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The kind of an actor lifecycle transition. */
public enum ActorLifecycleKind {
    CREATED(1),
    JOINED(2),
    LEFT(3),
    DISCONNECTED(4),
    DESTROYED(5);

    private final int value;

    ActorLifecycleKind(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ActorLifecycleKind fromValue(int value) {
        for (ActorLifecycleKind kind : values()) {
            if (kind.value == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException("invalid ActorLifecycleKind value: " + value);
    }
}
