package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.PollSourceKind;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import java.time.Duration;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MonitorPollingContractTest {
    private static final long READY_SLOT = 71L;
    private static final long DISCONNECTED_SLOT = 72L;

    @Test
    public void pollerTracksInprocMonitorReadiness() {
        assertMonitorReadiness(TestSupport.inprocEndpoint(
            "monitor-poller-contract"));
    }

    @Test
    public void pollerTracksTcpMonitorReadiness() {
        assertMonitorReadiness(TestSupport.tcpEndpoint());
    }

    @Test
    public void pollerRejectsUnsupportedMonitorEventsWithTypedError() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket client = context.createDealerSocket();
             SocketMonitor monitor = client.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             Poller poller = Zlink.createPoller()) {
            ZlinkConfigException pollout = assertThrows(
                ZlinkConfigException.class,
                () -> poller.add(monitor, READY_SLOT,
                    PollEventFlags.POLLOUT));
            assertEquals(ConfigResult.INVALID_ARGUMENT, pollout.getResult());

            ZlinkConfigException completion = assertThrows(
                ZlinkConfigException.class,
                () -> poller.add(monitor, READY_SLOT,
                    PollEventFlags.POLLCOMPLETION));
            assertEquals(ConfigResult.INVALID_ARGUMENT, completion.getResult());
            assertEquals(0, poller.size());
        }
    }

    private static void assertMonitorReadiness(String endpoint) {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket();
             SocketMonitor monitor = client.monitorOpen(
                 MonitorEventType.CONNECTION_READY,
                 MonitorEventType.DISCONNECTED);
             Poller poller = Zlink.createPoller()) {
            server.bind(endpoint);
            poller.add(monitor, READY_SLOT, PollEventFlags.POLLIN);
            poller.modify(monitor);
            client.connect(endpoint);

            PollEvents events = new PollEvents(1);
            assertEquals(0, poller.wait(events, Duration.ofMillis(25)));

            poller.modify(monitor, PollEventFlags.POLLIN);
            assertReady(events, poller, READY_SLOT);
            MonitorEvent ready = monitor.recv(RecvFlags.DONT_WAIT);
            assertNotNull(ready);
            assertEquals(MonitorEventType.CONNECTION_READY, ready.event());

            assertTrue(poller.remove(monitor));
            server.close();
            assertEquals(0, poller.wait(events, Duration.ofMillis(25)));

            poller.add(monitor, DISCONNECTED_SLOT, PollEventFlags.POLLIN);
            assertReady(events, poller, DISCONNECTED_SLOT);
            MonitorEvent disconnected = monitor.recv(RecvFlags.DONT_WAIT);
            assertNotNull(disconnected);
            assertEquals(MonitorEventType.DISCONNECTED,
                disconnected.event());
        }
    }

    private static void assertReady(PollEvents events, Poller poller,
                                    long slot) {
        assertEquals(1, poller.wait(events,
            Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
        assertEquals(PollSourceKind.SOCKET, events.sourceKind(0));
        assertEquals(slot, events.slot(0));
        assertTrue(events.hasEvent(0, PollEventFlags.POLLIN));
    }
}
