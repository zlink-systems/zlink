/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.perf.PerfUtil;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertThrows;

class PerfConnectionReadyCountTest {
    @Test
    void snapshotsDoNotAdmitMissingConnections() {
        SocketMonitor monitor = monitorWithCounts(1, 1, 1, 0);
        assertThrows(IllegalStateException.class, () -> awaitReady(monitor));
    }

    @Test
    void currentCountAdmitsAllRequestedConnections() {
        SocketMonitor monitor = monitorWithCounts(3);
        assertDoesNotThrow(() -> awaitReady(monitor));
    }

    private static void awaitReady(SocketMonitor monitor) {
        PerfUtil.waitForMonitorEvent(monitor, MonitorEventType.CONNECTION_READY,
            3, Duration.ofSeconds(1), "test connections");
    }

    private static SocketMonitor monitorWithCounts(long... counts) {
        ArrayDeque<MonitorEvent> events = new ArrayDeque<>();
        for (long count : counts) {
            events.add(new MonitorEvent(MonitorEventType.CONNECTION_READY,
                count, Optional.empty(), "", ""));
        }
        return (SocketMonitor) Proxy.newProxyInstance(
            SocketMonitor.class.getClassLoader(), new Class<?>[] {SocketMonitor.class},
            (proxy, method, args) -> {
                if (method.getName().equals("recv")) {
                    if (events.isEmpty()) {
                        throw new IllegalStateException("monitor events exhausted");
                    }
                    return events.remove();
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }
}
