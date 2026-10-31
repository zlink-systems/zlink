package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLaneTestProbe;

import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkPublishedNodeRoutesTest {
    @Test
    void fixedRouterSkipsBlockedSelectorButCandidateSelectionWaits() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var router = new Router();
        sockets.registerRouteRouter("orders", router.socket);
        try (var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            var blocked = new BlockedLane(sockets.operationLane());
            try {
                var candidate =
                        workers.submit(
                                () ->
                                        sockets.submitToChannel(
                                                "orders",
                                                null,
                                                null,
                                                (client, timeout) -> fail("unexpected client"),
                                                (node, timeout) -> fail("unexpected node"),
                                                failure -> {}));
                awaitQueued(sockets.operationLane());
                var fixed = workers.submit(() -> send(sockets, "orders"));
                fixed.get(1, TimeUnit.SECONDS).toCompletableFuture().join();
                assertEquals(1, router.calls.get());
                assertFalse(candidate.isDone());
                assertEquals(1, ZLinkStateLaneTestProbe.queuedTurnCount(sockets.operationLane()));
            } finally {
                blocked.close();
            }
        }
    }

    @Test
    void fixedNodeSkipsBlockedSelectorAndUsesDistinctOwner() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var owner = new ZLinkStateLane();
        sockets.registerSpotRouterNode("orders", node(owner));
        try (var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            var blocked = new BlockedLane(sockets.operationLane());
            try {
                var fixed =
                        workers.submit(
                                () ->
                                        sockets.submitToNode(
                                                "orders",
                                                null,
                                                null,
                                                (router, timeout) -> fail("unexpected router"),
                                                (node, timeout) -> {
                                                    assertSame(owner, ZLinkStateLane.current());
                                                    return CompletableFuture.completedFuture(
                                                            "submitted");
                                                },
                                                failure -> fail(failure)));
                assertEquals(
                        "submitted", fixed.get(1, TimeUnit.SECONDS).toCompletableFuture().join());
                assertEquals(0, ZLinkStateLaneTestProbe.queuedTurnCount(sockets.operationLane()));
            } finally {
                blocked.close();
            }
        }
    }

    @Test
    void concurrentPublicationRetainsRoutesAndBackendDisconnectErrors() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var stable = new Router();
        var changing = new Router();
        sockets.registerRouteRouter("changing", changing.socket);
        sockets.registerRouteRouter("stable", stable.socket);
        var start = new CountDownLatch(1);
        try (var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            var mutation =
                    workers.submit(
                            () -> {
                                await(start);
                                for (int i = 0; i < 100; i++) {
                                    sockets.bindRouteRouter("changing", "inproc://changing-" + i);
                                    changing.socket.disconnectPeer(RoutingId.from("target"));
                                    var router = new Router();
                                    String channel = "orders-" + i;
                                    sockets.registerRouteRouter(channel, router.socket);
                                    sockets.bindRouteRouter(channel, "inproc://" + channel);
                                    send(sockets, channel).toCompletableFuture().join();
                                    router.socket.disconnectPeer(RoutingId.from("target"));
                                    assertSame(
                                            router.unbound,
                                            assertThrows(
                                                            CompletionException.class,
                                                            () ->
                                                                    send(sockets, channel)
                                                                            .toCompletableFuture()
                                                                            .join())
                                                    .getCause());
                                    assertEquals(2, router.calls.get());
                                }
                            });
            var sends =
                    workers.submit(
                            () -> {
                                await(start);
                                for (int i = 0; i < 500; i++) {
                                    send(sockets, "stable").toCompletableFuture().join();
                                    try {
                                        send(sockets, "changing").toCompletableFuture().join();
                                    } catch (CompletionException failure) {
                                        assertSame(changing.unbound, failure.getCause());
                                    }
                                }
                            });
            start.countDown();
            mutation.get(10, TimeUnit.SECONDS);
            sends.get(10, TimeUnit.SECONDS);
            assertEquals(500, stable.calls.get());
            assertEquals(500, changing.calls.get());
        }
        assertInstanceOf(
                ZLinkConfigurationException.class,
                assertThrows(
                                CompletionException.class,
                                () -> send(sockets, "missing").toCompletableFuture().join())
                        .getCause());
    }

    private static CompletionStage<Void> send(ZLinkChannelSocketRegistry sockets, String channel) {
        return sockets.submitToNode(
                channel,
                null,
                null,
                (router, timeout) -> router.send(RoutingId.from("target"), List.of()),
                (node, timeout) -> fail("unexpected node"),
                failure -> {});
    }

    private static ZLinkInternalSpotNode node(ZLinkStateLane owner) {
        return (ZLinkInternalSpotNode)
                Proxy.newProxyInstance(
                        ZLinkInternalSpotNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalSpotNode.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("sourceTransportLane")) return owner;
                            throw new AssertionError("unexpected node call: " + method.getName());
                        });
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS));
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }

    private static void awaitQueued(ZLinkStateLane lane) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (ZLinkStateLaneTestProbe.queuedTurnCount(lane) == 0 && System.nanoTime() < deadline)
            Thread.yield();
        assertTrue(ZLinkStateLaneTestProbe.queuedTurnCount(lane) > 0);
    }

    private static final class BlockedLane implements AutoCloseable {
        private final CountDownLatch release = new CountDownLatch(1);
        private final CompletionStage<Void> turn;

        BlockedLane(ZLinkStateLane lane) {
            var entered = new CountDownLatch(1);
            turn =
                    lane.runAsync(
                            () -> {
                                entered.countDown();
                                await(release);
                                return null;
                            });
            await(entered);
        }

        public void close() {
            release.countDown();
            turn.toCompletableFuture().join();
        }
    }

    private static final class Router {
        final AtomicInteger calls = new AtomicInteger();
        final ZlinkSubmitException unbound = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        private boolean connected = true;
        final ZLinkBackendRouterSocket socket =
                (ZLinkBackendRouterSocket)
                        Proxy.newProxyInstance(
                                ZLinkBackendRouterSocket.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendRouterSocket.class},
                                (proxy, method, args) -> {
                                    synchronized (this) {
                                        return switch (method.getName()) {
                                            case "send" -> {
                                                calls.incrementAndGet();
                                                if (!connected) throw unbound;
                                                yield CompletableFuture.completedFuture(null);
                                            }
                                            case "bind" -> {
                                                connected = true;
                                                yield null;
                                            }
                                            case "disconnectPeer" -> {
                                                connected = false;
                                                yield null;
                                            }
                                            case "lastEndpoint" -> "inproc://test";
                                            default ->
                                                    throw new AssertionError(
                                                            "unexpected router call: "
                                                                    + method.getName());
                                        };
                                    }
                                });
    }
}
