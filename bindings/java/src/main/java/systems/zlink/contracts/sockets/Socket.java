/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;

/**
 * Common public contract for zlink socket resources.
 */
public interface Socket extends AutoCloseable {
    CommonSocketOptions options();

    SocketMonitor monitorOpen();

    SocketMonitor monitorOpen(MonitorEventType... events);

    /**
     * Opens a monitor with an exact queue HWM in bytes. Zero selects the Core
     * default.
     */
    SocketMonitor monitorOpen(long monitorHwmBytes,
                              MonitorEventType... events);

    void setTlsServer(String certPem, String keyPem, boolean requireClientCert);

    void setTlsClient(String caCertPem, String hostname, boolean trustSystem);

    @Override
    void close();
}
