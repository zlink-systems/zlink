/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.eventing.*;
import systems.zlink.contracts.messaging.*;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.EnumCodecs;
import systems.zlink.runtime.nativeapi.Native;

import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.function.Supplier;

class NativeConnectionAttemptsTest {
    private static final Duration WAIT = Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS);

    @Test
    void queryErrorsRemainConfigErrors() {
        ZlinkConfigException error =
                assertThrows(
                        ZlinkConfigException.class,
                        () -> Native.connectionAttemptsSnapshot(MemorySegment.NULL));
        assertEquals(ConfigResult.INVALID_HANDLE, error.getResult());
    }

    @Test
    void publicSurfaceEnumsAndOpaqueTokens() throws Exception {
        for (Class<?> type :
                List.of(
                        PairSocket.class,
                        DealerSocket.class,
                        RouterSocket.class,
                        PubSocket.class,
                        SubSocket.class,
                        XPubSocket.class,
                        XSubSocket.class)) {
            assertEquals(List.class, type.getMethod("connectionAttemptsSnapshot").getReturnType());
        }
        assertThrows(
                NoSuchMethodException.class,
                () -> StreamSocket.class.getMethod("connectionAttemptsSnapshot"));
        assertEquals(128, PollEventFlags.POLLCONNECT.mask());
        assertEquals(128, EnumCodecs.pollEventFlagValue(PollEventFlags.POLLCONNECT));
        assertTrue(EnumCodecs.pollEventFlagsFromMask(255).contains(PollEventFlags.POLLCONNECT));
        for (int i = 0; i < ConnectionAttemptState.values().length; i++)
            assertEquals(
                    ConnectionAttemptState.values()[i],
                    EnumCodecs.connectionAttemptStateFromValue(i));
        for (int i = 0; i < ConnectionFailureKind.values().length; i++)
            assertEquals(
                    ConnectionFailureKind.values()[i],
                    EnumCodecs.connectionFailureKindFromValue(i));
        for (int i = 0; i < ConnectionFailureStage.values().length; i++)
            assertEquals(
                    ConnectionFailureStage.values()[i],
                    EnumCodecs.connectionFailureStageFromValue(i));
        assertThrows(
                IllegalArgumentException.class,
                () -> EnumCodecs.connectionAttemptStateFromValue(3));
        assertThrows(
                IllegalArgumentException.class,
                () -> EnumCodecs.connectionFailureKindFromValue(10));
        assertThrows(
                IllegalArgumentException.class,
                () -> EnumCodecs.connectionFailureStageFromValue(7));
        ConnectionFailure failure =
                new ConnectionFailure(
                        ConnectionFailureKind.OTHER,
                        ConnectionFailureStage.TRANSPORT,
                        -1,
                        4294967295L);
        ConnectionAttempt row =
                new ConnectionAttempt(
                        "endpoint",
                        Long.MIN_VALUE,
                        -1,
                        Long.MIN_VALUE,
                        ConnectionAttemptState.ATTEMPT_FAILED,
                        failure);
        assertEquals(
                row,
                new ConnectionAttempt(
                        "endpoint",
                        Long.MIN_VALUE,
                        -1,
                        Long.MIN_VALUE,
                        ConnectionAttemptState.ATTEMPT_FAILED,
                        failure));
        assertEquals(4294967295L, row.lastFailure().disconnectReason());
        assertThrows(
                NullPointerException.class,
                () ->
                        new ConnectionAttempt(
                                null, 0, 0, 0, ConnectionAttemptState.CONNECTING, failure));
    }

    @Test
    void allSevenNativeContractsReturnEmptySnapshots() {
        try (Context c = Zlink.createContext();
                PairSocket pair = c.createPairSocket();
                DealerSocket dealer = c.createDealerSocket();
                RouterSocket router = c.createRouterSocket();
                PubSocket pub = c.createPubSocket();
                SubSocket sub = c.createSubSocket();
                XPubSocket xpub = c.createXPubSocket();
                XSubSocket xsub = c.createXSubSocket()) {
            for (Supplier<List<ConnectionAttempt>> getter :
                    List.<Supplier<List<ConnectionAttempt>>>of(
                            pair::connectionAttemptsSnapshot,
                            dealer::connectionAttemptsSnapshot,
                            router::connectionAttemptsSnapshot,
                            pub::connectionAttemptsSnapshot,
                            sub::connectionAttemptsSnapshot,
                            xpub::connectionAttemptsSnapshot,
                            xsub::connectionAttemptsSnapshot)) {
                List<ConnectionAttempt> snapshot = getter.get();
                assertTrue(snapshot.isEmpty());
                assertThrows(UnsupportedOperationException.class, () -> snapshot.add(null));
            }
        }
    }

    @Test
    void connectingUtf8ArenaCopyPollLevelAndNewIntent() {
        try (Context c = Zlink.createContext();
                PairSocket client = c.createPairSocket();
                PairSocket server = c.createPairSocket();
                Poller poller = Zlink.createPoller()) {
            client.options().linger(Duration.ZERO);
            server.options().linger(Duration.ZERO);
            String endpoint = TestSupport.inprocEndpoint("연결-é");
            poller.add(client, 17, PollEventFlags.POLLCONNECT);
            client.connect(endpoint);
            assertTrue(connectReady(poller));
            assertTrue(connectReady(poller));
            List<ConnectionAttempt> copied = client.connectionAttemptsSnapshot();
            ConnectionAttempt initial = copied.getFirst();
            assertEquals(endpoint, initial.endpoint());
            assertEquals(ConnectionAttemptState.CONNECTING, initial.state());
            assertEquals(0, initial.lastFailureId());
            assertNotEquals(0, initial.intentId());
            assertNotEquals(0, initial.revision());
            assertFalse(connectReady(poller));
            server.bind(endpoint);
            ConnectionAttempt connected =
                    waitState(
                            client::connectionAttemptsSnapshot,
                            poller,
                            ConnectionAttemptState.CONNECTED);
            assertEquals(initial.intentId(), connected.intentId());
            assertNotEquals(initial.revision(), connected.revision());
            assertFalse(connectReady(poller));
            client.disconnect(endpoint);
            assertTrue(connectReady(poller));
            assertTrue(client.connectionAttemptsSnapshot().isEmpty());
            assertFalse(connectReady(poller));
            client.connect(endpoint);
            ConnectionAttempt next = client.connectionAttemptsSnapshot().getFirst();
            assertNotEquals(initial.intentId(), next.intentId());
            assertEquals(endpoint, copied.getFirst().endpoint());
            assertEquals(ConnectionAttemptState.CONNECTING, copied.getFirst().state());
        }
    }

    @Test
    void refusedFailureIsNormalAndRecoveryRetainsFailure() {
        try (Context c = Zlink.createContext();
                DealerSocket client = c.createDealerSocket();
                RouterSocket server = c.createRouterSocket();
                Poller poller = Zlink.createPoller()) {
            client.options().linger(Duration.ZERO);
            server.options().linger(Duration.ZERO);
            server.bind("tcp://127.0.0.1:*");
            String endpoint = server.options().lastEndpoint();
            server.unbind(endpoint);
            poller.add(client, 1, PollEventFlags.POLLCONNECT);
            client.connect(endpoint);
            ConnectionAttempt failed =
                    waitState(
                            client::connectionAttemptsSnapshot,
                            poller,
                            ConnectionAttemptState.ATTEMPT_FAILED);
            assertEquals(ConnectionFailureKind.REFUSED, failed.lastFailure().kind());
            assertEquals(ConnectionFailureStage.TRANSPORT, failed.lastFailure().stage());
            assertNotEquals(0, failed.lastFailure().normalizedErrno());
            assertNotEquals(0, failed.lastFailureId());
            server.bind(endpoint);
            ConnectionAttempt recovered =
                    waitState(
                            client::connectionAttemptsSnapshot,
                            poller,
                            ConnectionAttemptState.CONNECTED);
            assertEquals(failed.intentId(), recovered.intentId());
            assertNotEquals(failed.revision(), recovered.revision());
            assertNotEquals(0, recovered.lastFailureId());
            assertEquals(ConnectionFailureKind.REFUSED, recovered.lastFailure().kind());
            assertFalse(connectReady(poller));
        }
    }

    @Test
    void twoBuffersGrowAndDuplicateEndpointsKeepDistinctIntents() {
        try (Context c = Zlink.createContext();
                DealerSocket client = c.createDealerSocket()) {
            client.options().linger(Duration.ZERO);
            List<String> expected = new ArrayList<>();
            String duplicate = TestSupport.inprocEndpoint("duplicate");
            expected.add(duplicate);
            expected.add(duplicate);
            for (int i = 0; i < 20; i++)
                expected.add(TestSupport.inprocEndpoint("buffer-" + i + "-é-" + "x".repeat(i * 3)));
            for (String endpoint : expected) client.connect(endpoint);
            List<ConnectionAttempt> rows = client.connectionAttemptsSnapshot();
            assertEquals(expected.size(), rows.size());
            assertEquals(
                    expected.stream().sorted().toList(),
                    rows.stream().map(ConnectionAttempt::endpoint).sorted().toList());
            assertEquals(
                    rows.size(),
                    new HashSet<>(rows.stream().map(ConnectionAttempt::intentId).toList()).size());
            assertThrows(UnsupportedOperationException.class, () -> rows.clear());
        }
    }

    @Test
    void combinedReadinessStillDrainsRequestCompletion() throws Exception {
        try (Context c = Zlink.createContext();
                RouterSocket client = c.createRouterSocket();
                RouterSocket server = c.createRouterSocket();
                Poller poller = Zlink.createPoller();
                Received received = new Received()) {
            client.options().linger(Duration.ZERO);
            server.options().linger(Duration.ZERO);
            server.options().recvTimeout(WAIT);
            String endpoint = TestSupport.inprocEndpoint("connect-completion");
            client.setRoutingId(RoutingId.from("client"));
            server.setRoutingId(RoutingId.from("server"));
            client.options().setConnectRoutingId(RoutingId.from("server"));
            server.bind(endpoint);
            poller.add(
                    client,
                    29,
                    PollEventFlags.POLLIN,
                    PollEventFlags.POLLOUT,
                    PollEventFlags.POLLCOMPLETION,
                    PollEventFlags.POLLROUTE,
                    PollEventFlags.POLLCONNECT);
            client.connect(endpoint);
            assertTrue(connectReady(poller));
            waitState(client::connectionAttemptsSnapshot, poller, ConnectionAttemptState.CONNECTED);
            var reply =
                    client.request(RoutingId.from("server"))
                            .message(Message.from("request"))
                            .timeout(WAIT)
                            .submit()
                            .reply()
                            .toCompletableFuture();
            assertTrue(server.recv(received, RecvFlags.NONE));
            received.reply().message(Message.from("reply")).submit();
            long deadline = System.nanoTime() + WAIT.toNanos();
            while (!reply.isDone()) {
                long remaining = deadline - System.nanoTime();
                assertTrue(remaining > 0, "request completion was not drained");
                poller.wait(new PollEvents(1), Duration.ofNanos(remaining));
            }
            List<Message> messages = reply.get();
            try {
                assertEquals("reply", messages.getFirst().toUtf8String());
            } finally {
                Message.closeAll(messages);
            }
        }
    }

    private static boolean connectReady(Poller poller) {
        PollEvents events = new PollEvents(1);
        int count = poller.wait(events, Duration.ZERO);
        return count == 1 && (events.revents(0) & PollEventFlags.POLLCONNECT.mask()) != 0;
    }

    private static ConnectionAttempt waitState(
            Supplier<List<ConnectionAttempt>> getter, Poller poller, ConnectionAttemptState state) {
        long deadline = System.nanoTime() + WAIT.toNanos();
        ConnectionAttempt row = getter.get().getFirst();
        PollEvents events = new PollEvents(1);
        while (row.state() != state) {
            long remaining = deadline - System.nanoTime();
            assertTrue(remaining > 0, "connection did not reach " + state);
            int count = poller.wait(events, Duration.ofNanos(remaining));
            if (count == 1 && (events.revents(0) & PollEventFlags.POLLCONNECT.mask()) != 0) {
                row = getter.get().getFirst();
            }
        }
        return row;
    }
}
