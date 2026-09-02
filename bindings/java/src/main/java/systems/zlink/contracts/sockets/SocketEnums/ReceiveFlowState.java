/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/**
 * A DEALER/ROUTER socket's local receive-flow state. Control uses the
 * Application connection for count-1 peers and the Completion connection for
 * count-2 ROUTER-ROUTER peers. Values match
 * {@code zlink_receive_flow_state_t}.
 */
public enum ReceiveFlowState {
    /** The socket accepts application traffic normally. */
    RUNNING(0),
    /** The socket asks its paired peers to stop sending application traffic. */
    PAUSED(1);

    private final int value;

    ReceiveFlowState(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static ReceiveFlowState fromValue(int value) {
        return switch (value) {
            case 0 -> RUNNING;
            case 1 -> PAUSED;
            default -> throw new IllegalArgumentException(
                "Invalid ReceiveFlowState value: " + value);
        };
    }
}
