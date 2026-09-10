package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

final class ZLinkChannelSubmissionFailureTest {
    private static final Duration TIMEOUT = Duration.ofSeconds(10);
    private static final RoutingId TARGET = RoutingId.from("submission-failure-target");

    @Test
    void routeRequestClosesPayloadWhenTheBackendRejectsSynchronously() {
        IllegalStateException rejection = new IllegalStateException("route rejected");
        ZLinkBackendRouterSocket router = routerThatThrows(rejection, new AtomicInteger());

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("route-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            try {
                CompletionException failure = assertThrows(CompletionException.class,
                    () -> new RouteRequestCall(runtime, "orders", sockets(router), TIMEOUT,
                        TARGET, payload, Optional.of("request"), null,
                        ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE, null)
                        .submit(String.class).toCompletableFuture().join());

                assertEquals(rejection, failure.getCause());
                assertTrue(payload.empty(), "failed submit must release the caller payload");
            } finally {
                close(runtime, List.of());
            }
        }
    }

    @Test
    void routeRequestClosesPayloadWhenRegistryCapacityRejectsBeforeBinding() {
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendRouterSocket router = routerThatThrows(
            new AssertionError("binding must not start after capacity rejection"), attempts);

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("route-capacity-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            List<CompletableFuture<Void>> pending = List.of();
            try {
                pending = exhaustCapacity(runtime);

                ZLinkFrameworkException failure = requestFailure(
                    new RouteRequestCall(runtime, "orders", sockets(router), TIMEOUT,
                        TARGET, payload, Optional.of("request"), null,
                        ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE, null)
                        .submit(String.class));

                assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, failure.kind());
                assertEquals(0, attempts.get());
                assertTrue(payload.empty(), "capacity rejection must release the caller payload");
            } finally {
                close(runtime, pending);
            }
        }
    }

    @Test
    void meshChannelRequestClosesPayloadWhenTheBackendRejectsSynchronously() {
        IllegalStateException rejection = new IllegalStateException("channel rejected");
        ZLinkInternalSpotNode node = channelNodeThatThrows(rejection, new AtomicInteger());

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("channel-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            try {
                CompletionException failure = assertThrows(CompletionException.class,
                    () -> new ChannelRequestCall(runtime, "orders", sockets(node), TIMEOUT, payload,
                        Optional.of("request"), TIMEOUT)
                        .submit(String.class).toCompletableFuture().join());

                assertEquals(rejection, failure.getCause());
                assertTrue(payload.empty(), "failed submit must release the caller payload");
            } finally {
                close(runtime, List.of());
            }
        }
    }

    @Test
    void meshChannelRequestClosesPayloadWhenRegistryCapacityRejectsBeforeBinding() {
        AtomicInteger attempts = new AtomicInteger();
        ZLinkInternalSpotNode node = channelNodeThatUsesCallerRegistry(attempts);

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("channel-capacity-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            List<CompletableFuture<Void>> pending = List.of();
            try {
                pending = exhaustCapacity(runtime);

                ZLinkFrameworkException failure = requestFailure(
                    new ChannelRequestCall(runtime, "orders", sockets(node), TIMEOUT, payload,
                        Optional.of("request"), TIMEOUT).submit(String.class));

                assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, failure.kind());
                assertEquals(0, attempts.get());
                assertTrue(payload.empty(), "capacity rejection must release the caller payload");
            } finally {
                close(runtime, pending);
            }
        }
    }

    @Test
    void meshNodeSendClosesPayloadWhenBackendRejectsSynchronously() {
        IllegalStateException rejection = new IllegalStateException(
            "node send rejected");
        AtomicInteger attempts = new AtomicInteger();
        ZLinkInternalSpotNode node = nodeSendThatThrows(rejection, attempts);

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("node-send-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            try {
                IllegalStateException failure = assertThrows(
                    IllegalStateException.class,
                    () -> new RouteSendCall(
                        runtime,
                        "orders",
                        sockets(node),
                        TARGET,
                        payload,
                        Optional.of("command"),
                        ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
                        ZLinkApplicationMetadata.empty()).submit());

                assertEquals(rejection, failure);
                assertEquals(1, attempts.get());
                assertTrue(payload.empty(),
                    "synchronous node rejection must release encoded parts");
            } finally {
                close(runtime, List.of());
            }
        }
    }

    @Test
    void meshChannelSendClosesPayloadWhenBackendRejectsSynchronously() {
        IllegalStateException rejection = new IllegalStateException(
            "channel send rejected");
        AtomicInteger attempts = new AtomicInteger();
        ZLinkInternalSpotNode node = channelSendThatThrows(
            rejection, attempts);

        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             Message payload = Message.from("channel-send-payload")) {
            ZLinkChannelCallRuntime runtime = runtime(scheduler);
            try {
                IllegalStateException failure = assertThrows(
                    IllegalStateException.class,
                    () -> new ChannelSendCall(
                        runtime,
                        "orders",
                        sockets(node),
                        TIMEOUT,
                        payload,
                        Optional.of("command")).submit());

                assertEquals(rejection, failure);
                assertEquals(1, attempts.get());
                assertTrue(payload.empty(),
                    "synchronous channel rejection must release encoded parts");
            } finally {
                close(runtime, List.of());
            }
        }
    }

    private static ZLinkChannelSocketRegistry sockets(ZLinkInternalSpotNode node) {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        sockets.registerSpotRouterNode("orders", node);
        return sockets;
    }

    private static ZLinkChannelSocketRegistry sockets(ZLinkBackendRouterSocket router) {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        sockets.registerRouteRouter("orders", router);
        return sockets;
    }

    private static ZLinkChannelCallRuntime runtime(
        java.util.concurrent.ScheduledExecutorService scheduler) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkMessageFlowTracer flow = new ZLinkMessageFlowTracer(
            options.registration().dispatchOptions(), null, Runnable::run);
        return new ZLinkChannelCallRuntime(flow, scheduler, null, null, null);
    }

    private static List<CompletableFuture<Void>> exhaustCapacity(
        ZLinkChannelCallRuntime runtime) {
        List<CompletableFuture<Void>> pending = new ArrayList<>(
            ZLinkServiceOperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS);
        for (int index = 0;
             index < ZLinkServiceOperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS;
             index++) {
            pending.add(runtime.submit(TIMEOUT, CompletableFuture<Void>::new, ignored -> { }));
        }
        return pending;
    }

    private static void close(
        ZLinkChannelCallRuntime runtime,
        List<? extends CompletableFuture<?>> pending) {
        runtime.beginClose();
        for (CompletableFuture<?> operation : pending) {
            try {
                operation.join();
            } catch (CompletionException ignored) {
                // Closing settles the registry entries exceptionally on its dispatcher.
            }
        }
    }

    private static ZLinkFrameworkException requestFailure(CompletionStage<?> request) {
        CompletionException failure = assertThrows(CompletionException.class,
            () -> request.toCompletableFuture().join());
        return assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
    }

    private static ZLinkBackendRouterSocket routerThatThrows(
        Throwable failure,
        AtomicInteger attempts) {
        return (ZLinkBackendRouterSocket) Proxy.newProxyInstance(
            ZLinkChannelSubmissionFailureTest.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendRouterSocket.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("request")) {
                    attempts.incrementAndGet();
                    throw failure;
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static ZLinkInternalSpotNode channelNodeThatThrows(
        Throwable failure,
        AtomicInteger attempts) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkChannelSubmissionFailureTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestToChannel")) {
                    attempts.incrementAndGet();
                    throw failure;
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static ZLinkInternalSpotNode channelNodeThatUsesCallerRegistry(
        AtomicInteger attempts) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkChannelSubmissionFailureTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestToChannel")) {
                    @SuppressWarnings("unchecked")
                    ZLinkServiceOperationRegistry operations =
                        (ZLinkServiceOperationRegistry) arguments[4];
                    java.util.UUID operationId = (java.util.UUID) arguments[5];
                    return operations.submit(operationId, (Duration) arguments[3], () -> {
                        attempts.incrementAndGet();
                        return new CompletableFuture<>();
                    }, ignored -> { });
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static ZLinkInternalSpotNode nodeSendThatThrows(
        Throwable failure,
        AtomicInteger attempts) {
        RoutingId source = RoutingId.from("submission-failure-source");
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkChannelSubmissionFailureTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> source;
                case "classifyNodeSendTarget" -> Optional.empty();
                case "sendToNode" -> {
                    attempts.incrementAndGet();
                    throw failure;
                }
                default -> defaultValue(method.getReturnType());
            });
    }

    private static ZLinkInternalSpotNode channelSendThatThrows(
        Throwable failure,
        AtomicInteger attempts) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkChannelSubmissionFailureTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("sendToChannel")) {
                    attempts.incrementAndGet();
                    throw failure;
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == long.class) {
            return 0L;
        }
        throw new AssertionError("unexpected primitive proxy method: " + type);
    }
}
