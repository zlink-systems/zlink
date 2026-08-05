/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Reports what triggered the last automatic high-water-mark recalculation. */
public enum AutoHwmRecalcReason {
    /** No recalculation has occurred yet. */
    NONE(0),
    /** First recalculation after context creation. */
    INITIAL(1),
    /** A socket's topology role changed. */
    ROLE_CHANGE(2),
    /** Auto-HWM was enabled or disabled. */
    POLICY_TOGGLE(3),
    /** An explicit recalculation was requested. */
    REFRESH(4),
    /** A scheduled deferred queue shrink fired. */
    DEFERRED_SHRINK(5);

    private final int value;

    AutoHwmRecalcReason(int value) {
        this.value = value;
    }

    int value() {
        return value;
    }

    static AutoHwmRecalcReason fromValue(int value) {
        return switch (value) {
            case 0 -> NONE;
            case 1 -> INITIAL;
            case 2 -> ROLE_CHANGE;
            case 3 -> POLICY_TOGGLE;
            case 4 -> REFRESH;
            case 5 -> DEFERRED_SHRINK;
            default -> throw new IllegalArgumentException(
                "Invalid AutoHwmRecalcReason value: " + value);
        };
    }
}
