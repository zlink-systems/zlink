package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.function.Supplier;

final class ZLinkNodeOperationOwnerTest {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final RoutingId TARGET = RoutingId.from("target");

    @Test
    void oneChannelCallerUsesIndependentNodeOwners() throws Exception {
        var a = new ZLinkStateLane();
        var b = new ZLinkStateLane();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var calls = new ZLinkChannelCallRuntime(null, null, null, null);
        try (var registryA = new ZLinkServiceOperationRegistry(a, shutdown());
                var registryB = new ZLinkServiceOperationRegistry(b, shutdown())) {
            var held =
                    a.runAsync(
                            () -> {
                                entered.countDown();
                                try {
                                    release.await();
                                } catch (InterruptedException failure) {
                                    throw new AssertionError(failure);
                                }
                            });
            try {
                assertTrue(entered.await(1, TimeUnit.SECONDS));
                var nodeA = node(registryA, a, () -> CompletableFuture.completedFuture(reply()));
                var nodeB = node(registryB, b, () -> CompletableFuture.completedFuture(reply()));
                var pendingA =
                        new ZLinkStateLane()
                                .runAsync(
                                        () ->
                                                calls.requestNode(
                                                        UUID.randomUUID(),
                                                        nodeA,
                                                        TARGET,
                                                        new byte[0],
                                                        List.of(),
                                                        TIMEOUT,
                                                        new CompletableFuture<>()))
                                .toCompletableFuture()
                                .get(1, TimeUnit.SECONDS);
                var completedB =
                        calls.requestNode(
                                UUID.randomUUID(),
                                nodeB,
                                TARGET,
                                new byte[0],
                                List.of(),
                                TIMEOUT,
                                new CompletableFuture<>());
                completedB.toCompletableFuture().get(1, TimeUnit.SECONDS).close();
                assertFalse(pendingA.toCompletableFuture().isDone());
                release.countDown();
                pendingA.toCompletableFuture().get(1, TimeUnit.SECONDS).close();
            } finally {
                release.countDown();
                held.toCompletableFuture().get(1, TimeUnit.SECONDS);
                calls.beginClose();
            }
        }
    }

    @Test
    void channelShutdownSettlesTheNodeEntryAndDiscardsItsLateReply() throws Exception {
        var lane = new ZLinkStateLane();
        var closed = shutdown();
        var binding = new CompletableFuture<ZLinkBackendReceived>();
        var entered = new CountDownLatch(1);
        var calls = new ZLinkChannelCallRuntime(null, null, null, null);
        var sockets = new ZLinkChannelSocketRegistry();
        try (var operations = new ZLinkServiceOperationRegistry(lane, closed)) {
            var node =
                    node(
                            operations,
                            lane,
                            () -> {
                                entered.countDown();
                                return binding;
                            });
            sockets.registerSpotRouterNode("node", node);
            var request =
                    calls.requestNode(
                            UUID.randomUUID(),
                            node,
                            TARGET,
                            new byte[0],
                            List.of(),
                            TIMEOUT,
                            new CompletableFuture<>());
            assertTrue(entered.await(1, TimeUnit.SECONDS));
            calls.beginClose();
            sockets.rejectPendingAdmission();
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () -> request.toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertSame(closed, failure.getCause());
            assertEquals(0, operations.pendingCount());
            assertTrue(operations.isClosed(), "shutdown also rejects queued registrations");
            Message payload = Message.from("late");
            binding.complete(
                    new ZLinkBackendReceived(
                            ZLinkBackendRequestResult.OK,
                            Optional.empty(),
                            Optional.empty(),
                            Optional.empty(),
                            List.of(payload)));
            assertTrue(payload.empty());
        } finally {
            calls.beginClose();
            sockets.closeAll();
        }
    }

    private static ZLinkInternalSpotNode node(
            ZLinkServiceOperationRegistry operations,
            ZLinkStateLane lane,
            Supplier<CompletionStage<ZLinkBackendReceived>> binding) {
        return (ZLinkInternalSpotNode)
                Proxy.newProxyInstance(
                        ZLinkInternalSpotNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalSpotNode.class},
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "operationRegistry" -> operations;
                                    case "sourceTransportLane" -> lane;
                                    case "requestToNode" -> {
                                        assertSame(
                                                operations,
                                                args[4],
                                                "the source node owns the pending entry");
                                        yield operations.submit(
                                                (UUID) args[5],
                                                (Duration) args[3],
                                                () -> {
                                                    assertTrue(lane.isOnLane());
                                                    assertEquals(1, operations.pendingCount());
                                                    return binding.get();
                                                },
                                                ZLinkBackendReceived::close);
                                    }
                                    default -> throw new AssertionError(method.getName());
                                });
    }

    private static ZLinkBackendReceived reply() {
        return new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of());
    }

    private static ZLinkFrameworkException shutdown() {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.SHUTTING_DOWN, "node closed");
    }
}
