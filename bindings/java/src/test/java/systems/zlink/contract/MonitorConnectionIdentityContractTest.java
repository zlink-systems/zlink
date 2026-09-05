package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventFlags;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Predicate;

import org.junit.jupiter.api.Disabled;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MonitorConnectionIdentityContractTest {
    private static final int APPLICATION_LANE = 0;

    @Test
    public void tcpReadyAndDisconnectedKeepIdentityAcrossReconnect() {
        verifyReadyAndDisconnectedIdentity(TestSupport.tcpEndpoint());
    }

    @Disabled("Core emits inproc READY and DISCONNECTED with different identities")
    @Test
    public void inprocReadyAndDisconnectedKeepIdentityAcrossReconnect() {
        verifyReadyAndDisconnectedIdentity(TestSupport.inprocEndpoint(
            "monitor-connection-identity"));
    }

    @Disabled("Core emits TCP CLOSED with a new endpoint identity")
    @Test
    public void tcpClosedKeepsReadyConnectionIdentity() {
        verifyClosedIdentity(TestSupport.tcpEndpoint());
    }

    @Disabled("Core does not emit CLOSED for an inproc peer shutdown")
    @Test
    public void inprocClosedKeepsReadyConnectionIdentity() {
        verifyClosedIdentity(TestSupport.inprocEndpoint(
            "monitor-closed-identity"));
    }

    private static void verifyReadyAndDisconnectedIdentity(String endpoint) {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket client = context.createDealerSocket();
             SocketMonitor monitor = client.monitorOpen(
                 MonitorEventType.CONNECTION_READY,
                 MonitorEventType.DISCONNECTED,
                 MonitorEventType.CLOSED)) {
            client.setRoutingId(RoutingId.from("monitor-identity-client"));
            client.options().linger(Duration.ZERO);
            client.options().reconnectInterval(Duration.ofMillis(20));

            RouterSocket firstServer = openServer(context, endpoint);
            client.connect(endpoint);
            MonitorEvent firstReady = awaitReadyEdge(monitor);
            assertReadyIdentity(firstReady);

            firstServer.close();
            MonitorEvent disconnected = awaitEvent(monitor,
                MonitorEventType.DISCONNECTED);
            assertSameIdentity(firstReady, disconnected);

            try (RouterSocket replacement = openServer(context, endpoint)) {
                MonitorEvent reconnectedReady = awaitReadyEdge(monitor);
                assertReadyIdentity(reconnectedReady);
                assertNotEquals(firstReady.connectionId(),
                    reconnectedReady.connectionId(),
                    () -> "reconnect reused physical identity: first="
                        + firstReady + ", replacement=" + reconnectedReady);
            }
        }
    }

    private static void verifyClosedIdentity(String endpoint) {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket client = context.createDealerSocket();
             SocketMonitor monitor = client.monitorOpen(
                 MonitorEventType.CONNECTION_READY,
                 MonitorEventType.DISCONNECTED,
                 MonitorEventType.CLOSED)) {
            client.setRoutingId(RoutingId.from("monitor-closed-client"));
            client.options().linger(Duration.ZERO);
            client.options().reconnectInterval(Duration.ofMillis(20));

            RouterSocket server = openServer(context, endpoint);
            client.connect(endpoint);
            MonitorEvent ready = awaitReadyEdge(monitor);
            server.close();
            MonitorEvent disconnected = awaitEvent(monitor,
                MonitorEventType.DISCONNECTED);
            assertSameIdentity(ready, disconnected);
            assertSameIdentity(ready,
                awaitEvent(monitor, MonitorEventType.CLOSED));
        }
    }

    private static RouterSocket openServer(Context context, String endpoint) {
        RouterSocket server = context.createRouterSocket();
        server.options().linger(Duration.ZERO);
        server.bind(endpoint);
        return server;
    }

    private static void assertReadyIdentity(MonitorEvent event) {
        assertTrue(event.connectionId() != 0,
            () -> "READY has zero connectionId: " + event);
        assertEquals(APPLICATION_LANE, event.transportLane(),
            () -> "READY has unexpected lane: " + event);
        assertTrue((event.flags()
            & MonitorEventFlags.CONNECTION_READY_EDGE.mask()) != 0,
            () -> "READY edge flag was not mapped: " + event);
    }

    private static void assertSameIdentity(MonitorEvent expected,
                                           MonitorEvent actual) {
        assertTrue(actual.connectionId() != 0,
            () -> actual.event() + " has zero connectionId: " + actual);
        assertEquals(expected.connectionId(), actual.connectionId(),
            () -> "connection identity changed between events: expected="
                + expected + ", actual=" + actual);
        assertEquals(expected.transportLane(), actual.transportLane(),
            () -> "transport lane changed between events: expected="
                + expected + ", actual=" + actual);
    }

    private static MonitorEvent awaitEvent(SocketMonitor monitor,
                                           MonitorEventType expected) {
        return awaitMatchingEvent(monitor, expected,
            event -> event.event() == expected);
    }

    private static MonitorEvent awaitReadyEdge(SocketMonitor monitor) {
        return awaitMatchingEvent(monitor, MonitorEventType.CONNECTION_READY,
            event -> event.event() == MonitorEventType.CONNECTION_READY
                && (event.flags()
                    & MonitorEventFlags.CONNECTION_READY_EDGE.mask()) != 0);
    }

    private static MonitorEvent awaitMatchingEvent(SocketMonitor monitor,
                                                    MonitorEventType expected,
                                                    Predicate<MonitorEvent> predicate) {
        long deadline = System.nanoTime()
            + Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS).toNanos();
        List<MonitorEvent> observed = new ArrayList<>();
        while (System.nanoTime() < deadline) {
            MonitorEvent event = monitor.recv(RecvFlags.DONT_WAIT);
            if (event == null) {
                LockSupport.parkNanos(1_000_000L);
                continue;
            }
            observed.add(event);
            if (predicate.test(event)) {
                return event;
            }
        }
        throw new AssertionError("monitor event timed out: expected="
            + expected + ", observed=" + observed);
    }
}
