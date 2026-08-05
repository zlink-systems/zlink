/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Kind of event delivered by a MeshNode monitor. */
public enum MeshMonitorEventKind {
    STATE_CHANGED(1), PEER_CONNECTING(2), PEER_ADMITTED(3), PEER_DRAINING(4),
    PEER_CLOSED(5), PEER_REJECTED(6), CHANNEL_CHANGED(7),
    MESSAGE_SUBMITTED(8), BACKPRESSURED(11), OPERATION_COMPLETED(12), PROTOCOL_ERROR(13),
    CLAIM_REVOKED(14);

    private final int value;
    MeshMonitorEventKind(int value) { this.value = value; }
    public int value() { return value; }
    public static MeshMonitorEventKind fromValue(int value) {
        for (MeshMonitorEventKind kind : values()) {
            if (kind.value == value) return kind;
        }
        throw new IllegalArgumentException("invalid MeshMonitorEventKind value: " + value);
    }
}
