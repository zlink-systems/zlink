package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterRoute;
import systems.zlink.contracts.sockets.RouterSocket;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

final class ZLinkRouteMeshInboundIdentityIntegrationTest {
    @Test
    void nonInitiatorCanRequestBackByInboundProbeIdentityAndRelearnAfterReconnect()
            throws Exception {
        assumeNative();

        RoutingId initiatorRid = RoutingId.from("route-a-initiator");
        RoutingId listenerRid = RoutingId.from("route-z-listener");
        String endpoint = "inproc://java-route-inbound-identity-" + System.nanoTime();
        try (Context context = Zlink.createContext();
                RouterSocket listener = context.createRouterSocket();
                RouterSocket initiator = context.createRouterSocket();
                SocketMonitor listenerMonitor =
                        listener.monitorOpen(
                                MonitorEventType.DISCONNECTED, MonitorEventType.CONNECTION_READY);
                Poller listenerPoller = Zlink.createPoller();
                ExecutorService responder =
                        Executors.newSingleThreadExecutor(
                                task -> {
                                    Thread thread =
                                            new Thread(
                                                    task, "zlink-route-inbound-identity-responder");
                                    thread.setDaemon(true);
                                    return thread;
                                })) {
            // A failed assertion must not leave an inproc probe/request queued forever
            // while NativeContext.close() waits for the socket linger interval.
            listener.options().linger(Duration.ZERO);
            initiator.options().linger(Duration.ZERO);
            listener.setRoutingId(listenerRid);
            listener.bind(endpoint);
            initiator.setRoutingId(initiatorRid);
            listenerPoller.add(
                    listener,
                    0,
                    PollEventFlags.POLLIN,
                    PollEventFlags.POLLROUTE,
                    PollEventFlags.POLLCOMPLETION);
            listenerPoller.add(listenerMonitor, 1, PollEventFlags.POLLIN);
            PollEvents listenerEvents = new PollEvents(2);

            connectWithProbe(initiator, listenerRid, endpoint);
            long firstRouteGeneration =
                    awaitInboundProbeAndReadyRoute(
                            listener,
                            listenerMonitor,
                            listenerPoller,
                            listenerEvents,
                            initiatorRid,
                            0L);
            assertTrue(listenerPoller.remove(listenerMonitor));
            listenerPoller.modify(listener, PollEventFlags.POLLCOMPLETION);
            assertReverseRequest(
                    listener,
                    initiator,
                    responder,
                    initiatorRid,
                    "first",
                    listenerPoller,
                    listenerEvents);

            listenerPoller.add(listenerMonitor, 1, PollEventFlags.POLLIN);
            listenerPoller.modify(listener, PollEventFlags.POLLCOMPLETION);
            initiator.disconnect(endpoint);
            awaitDisconnected(listenerMonitor, listenerPoller, listenerEvents);
            listenerPoller.modify(
                    listener,
                    PollEventFlags.POLLIN,
                    PollEventFlags.POLLROUTE,
                    PollEventFlags.POLLCOMPLETION);
            connectWithProbe(initiator, listenerRid, endpoint);
            awaitInboundProbeAndReadyRoute(
                    listener,
                    listenerMonitor,
                    listenerPoller,
                    listenerEvents,
                    initiatorRid,
                    firstRouteGeneration);
            assertTrue(listenerPoller.remove(listenerMonitor));
            listenerPoller.modify(listener, PollEventFlags.POLLCOMPLETION);
            assertReverseRequest(
                    listener,
                    initiator,
                    responder,
                    initiatorRid,
                    "second",
                    listenerPoller,
                    listenerEvents);
        }
    }

    private static void connectWithProbe(
            RouterSocket initiator, RoutingId listenerRid, String endpoint) {
        initiator.options().setConnectRoutingId(listenerRid);
        initiator.options().probe(true);
        initiator.connect(endpoint);
    }

    private static long awaitInboundProbeAndReadyRoute(
            RouterSocket listener,
            SocketMonitor monitor,
            Poller poller,
            PollEvents events,
            RoutingId peerRid,
            long previousGeneration) {
        boolean probeReceived = false;
        boolean connectionReady = false;
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        try (Received received = new Received()) {
            while (System.nanoTime() < deadline) {
                MonitorEvent event;
                while ((event = monitor.recv(RecvFlags.DONT_WAIT)) != null) {
                    if (event.event() == MonitorEventType.CONNECTION_READY
                            && event.routingId().filter(peerRid::equals).isPresent()) {
                        connectionReady = true;
                    }
                }

                long generation = routeGeneration(listener, peerRid);
                if (probeReceived
                        && connectionReady
                        && generation != 0L
                        && generation != previousGeneration) {
                    return generation;
                }

                int ready =
                        poller.wait(
                                events,
                                Duration.ofNanos(Math.max(1, deadline - System.nanoTime())));
                if (ready == 0) break;
                if (!probeReceived
                        && events.slot(0) == 0
                        && events.hasEvent(0, PollEventFlags.POLLIN)) {
                    assertTrue(listener.recv(received, RecvFlags.DONT_WAIT));
                    assertEquals(peerRid, received.getRoutingId().orElseThrow());
                    assertEquals(1, received.parts().size());
                    assertEquals(0, received.parts().get(0).size());
                    probeReceived = true;
                }
            }
        }
        throw new AssertionError(
                "inbound probe and ready selected route were not observed for " + peerRid);
    }

    private static long routeGeneration(RouterSocket listener, RoutingId peerRid) {
        for (RouterRoute route : listener.routesSnapshot()) {
            if (route.routingId().equals(peerRid)) return route.routeGeneration();
        }
        return 0L;
    }

    private static void assertReverseRequest(
            RouterSocket requester,
            RouterSocket responderSocket,
            ExecutorService responder,
            RoutingId targetRid,
            String marker,
            Poller completions,
            PollEvents events)
            throws Exception {
        CompletableFuture<Void> server =
                CompletableFuture.runAsync(
                        () -> {
                            try (Received received = new Received()) {
                                responderSocket.recv(received, RecvFlags.NONE);
                                assertEquals(targetRid, responderSocket.getRoutingId());
                                assertArrayEquals(
                                        ("request:" + marker).getBytes(StandardCharsets.UTF_8),
                                        received.singlePartOrThrow().toByteArray());
                                received.reply()
                                        .message(Message.from(("reply:" + marker).getBytes()))
                                        .submit();
                            }
                        },
                        responder);

        List<Message> reply;
        CompletableFuture<List<Message>> pending;
        try {
            pending =
                    requester
                            .request(targetRid)
                            .message(Message.from(("request:" + marker).getBytes()))
                            .timeout(Duration.ofSeconds(2))
                            .submit()
                            .reply()
                            .toCompletableFuture();
        } catch (ZlinkSubmitException failure) {
            throw new AssertionError(
                    "request submit failed for "
                            + marker
                            + ": result="
                            + failure.getResult()
                            + ", nativeErrno="
                            + failure.getNativeErrno(),
                    failure);
        }
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (!pending.isDone() && System.nanoTime() < deadline) {
            completions.wait(events, Duration.ofNanos(Math.max(1, deadline - System.nanoTime())));
        }
        reply = pending.get(0, TimeUnit.SECONDS);
        try {
            assertEquals(1, reply.size());
            assertArrayEquals(
                    ("reply:" + marker).getBytes(StandardCharsets.UTF_8),
                    reply.get(0).toByteArray());
        } finally {
            Message.closeAll(reply);
        }
        server.get(3, TimeUnit.SECONDS);
    }

    private static void awaitDisconnected(SocketMonitor monitor, Poller poller, PollEvents events) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            MonitorEvent event;
            while ((event = monitor.recv(RecvFlags.DONT_WAIT)) != null) {
                if (event.event() == MonitorEventType.DISCONNECTED) return;
            }
            int ready =
                    poller.wait(
                            events, Duration.ofNanos(Math.max(1, deadline - System.nanoTime())));
            if (ready == 0) break;
        }
        throw new AssertionError("monitor did not report " + MonitorEventType.DISCONNECTED);
    }

    private static void assumeNative() {
        try {
            Zlink.version();
        } catch (Throwable error) {
            throw new AssertionError(
                    "zlink native library not found: " + error.getMessage(), error);
        }
    }
}
