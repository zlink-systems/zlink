/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** How a mesh peer connection was established. */
public enum MeshPeerSource {
    MANUAL(1),
    DISCOVERY(2),
    MIXED(3);

    private final int value;

    MeshPeerSource(int value) {
        this.value = value;
    }

    public int value() {
        return value;
    }

    public static MeshPeerSource fromValue(int value) {
        for (MeshPeerSource source : values()) {
            if (source.value == value) {
                return source;
            }
        }
        throw new IllegalArgumentException("invalid MeshPeerSource value: " + value);
    }
}
