/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import systems.zlink.contracts.sockets.RecvFlags;

/**
 * Observes a socket's connection lifecycle events and current status.
 * The caller owns this resource and must close it.
 */
public interface SocketMonitor extends AutoCloseable {
    SocketMonitorHandler IGNORE_HANDLER = event -> {
    };

    void onEvent(SocketMonitorHandler handler);

    MonitorEvent recv();

    MonitorEvent recv(RecvFlags flags);

    MonitorStatus status();

    @Override
    void close();
}
