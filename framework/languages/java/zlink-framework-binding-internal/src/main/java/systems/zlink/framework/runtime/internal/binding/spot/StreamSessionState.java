/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Lifecycle state of a STREAM session service. */
enum StreamSessionState {
    CREATED(1),
    STARTED(2),
    DRAINING(3),
    STOPPED(4),
    ERROR(5);

    private final int value;

    StreamSessionState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static StreamSessionState fromValue(int value) {
        for (StreamSessionState state : values()) {
            if (state.value == value) {
                return state;
            }
        }
        throw new IllegalArgumentException("invalid StreamSessionState value: " + value);
    }
}
