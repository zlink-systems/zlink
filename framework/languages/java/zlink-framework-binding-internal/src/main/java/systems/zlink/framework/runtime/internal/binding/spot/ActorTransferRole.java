/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The local role of a Core actor transfer fence. */
public enum ActorTransferRole {
    SOURCE(1),
    TARGET(2);

    private final int value;

    ActorTransferRole(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ActorTransferRole fromValue(int value) {
        for (ActorTransferRole role : values()) {
            if (role.value == value) {
                return role;
            }
        }
        throw new IllegalArgumentException("invalid ActorTransferRole value: " + value);
    }
}
