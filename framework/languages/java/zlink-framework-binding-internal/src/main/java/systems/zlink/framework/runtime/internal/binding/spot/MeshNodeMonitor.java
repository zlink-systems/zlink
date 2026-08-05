/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.sockets.RecvFlags;

/** Pull receiver for one MeshNode monitor stream. */
public interface MeshNodeMonitor extends AutoCloseable {
    MeshMonitorEvent recv(RecvFlags flags);
    default MeshMonitorEvent recv() { return recv(RecvFlags.NONE); }
    MeshMonitorStatus status();
    @Override void close();
}
