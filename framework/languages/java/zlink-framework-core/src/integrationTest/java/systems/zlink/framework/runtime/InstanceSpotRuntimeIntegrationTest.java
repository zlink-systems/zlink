package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.net.ServerSocket;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.actors.ZLinkActor;

final class InstanceSpotRuntimeIntegrationTest {
    @Test
    void publicRequestColdActivatesApplicationInstanceOnRemoteNode()
        throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.sends.set(0);
        EchoInstanceSpot.closes.set(null);
        SourceEntrySpot.reset();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        var store = new ZLinkInMemoryLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(
            Duration.ofMillis(20));
        var targetNode = targetOptions.addRouteMesh("game");
        targetNode.listen(targetEndpoint)
            .setRoutingId(RoutingId.from("instance-target-" + suffix));
        targetNode.objects().server().addInstanceSpotFactory(
            "EchoInstance",
            EchoInstanceSpot.class,
            factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(
            Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode.listen(sourceEndpoint)
            .setRoutingId(RoutingId.from("instance-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(SourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target = RuntimeTestSupport.startFramework(
                 targetOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime source = RuntimeTestSupport.startFramework(
                 sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            SourceEntrySpot.request.set(new Request("echo-" + suffix));
            SourceEntrySpot.start.complete(null);
            String reply = SourceEntrySpot.reply.get();

            assertEquals("echo:hello|echo:again|echo:after-close", reply);
            assertEquals(2, EchoInstanceSpot.initializations.get());
            assertEquals(1, EchoInstanceSpot.sends.get());
            assertTrue(EchoInstanceSpot.closes.get());
        }
    }

    @Test
    void publicRequestReactivatesInstanceSpotAfterIdleEviction()
        throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.sends.set(0);
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closeReason.set(null);
        EchoInstanceSpot.idleEvicted = new CompletableFuture<>();
        IdleSourceEntrySpot.reset();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        var store = new ZLinkInMemoryLocationStore();
        String spotId = "idle-" + suffix;
        IdleSourceEntrySpot.store = store;
        IdleSourceEntrySpot.spotId = spotId;

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(
            Duration.ofMillis(20));
        var targetNode = targetOptions.addRouteMesh("game");
        targetNode.listen(targetEndpoint)
            .setRoutingId(RoutingId.from("instance-idle-target-" + suffix))
            .setInstanceSpotIdleTimeout(Duration.ofMillis(100));
        targetNode.objects().server().addInstanceSpotFactory(
            "EchoInstance",
            EchoInstanceSpot.class,
            factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(
            Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode.listen(sourceEndpoint)
            .setRoutingId(RoutingId.from("instance-idle-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(IdleSourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target = RuntimeTestSupport.startFramework(
                 targetOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime source = RuntimeTestSupport.startFramework(
                 sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            IdleSourceEntrySpot.request.set(new Request(spotId));
            IdleSourceEntrySpot.start.complete(null);
            String reply = IdleSourceEntrySpot.reply.get(10, TimeUnit.SECONDS);

            assertEquals("echo:hello|echo:after-idle", reply);
            assertEquals(2, EchoInstanceSpot.initializations.get());
            assertEquals(ZLinkSpotCloseReason.IDLE_EVICTED,
                EchoInstanceSpot.closeReason.get());
        }
    }

    private record Request(String spotId) {}

    private record Warmup(String value) {}

    private record CloseInstance() {}

    public static final class SourceEntrySpot
        implements ZLinkEntrySpot<ZLinkActor> {
        static CompletableFuture<Void> start;
        static java.util.concurrent.atomic.AtomicReference<Request> request;
        static CompletableFuture<String> reply;
        private final ZLinkEntrySpotContext context;

        public SourceEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        static void reset() {
            start = new CompletableFuture<>();
            request = new java.util.concurrent.atomic.AtomicReference<>();
            reply = new CompletableFuture<>();
        }

        @Override public ZLinkEntrySpotContext context() { return context; }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return start.thenCompose(ignored -> {
                CompletionStage<Void> warmup = context.outbound()
                    .sendToSpot(request.get().spotId(), new Warmup("warmup"))
                    .instanceSpot("EchoInstance")
                    .inMesh("game")
                    .submit();
                CompletionStage<String> first = warmup.thenCompose(
                    sendCompleted -> context.outbound()
                        .requestToSpot(request.get().spotId(), "hello")
                        .instanceSpot("EchoInstance")
                        .inMesh("game")
                        .timeout(Duration.ofSeconds(5))
                        .submit(String.class));
                CompletionStage<String> firstAndSecond = first.thenCompose(
                    firstValue -> {
                        return context.outbound()
                            .requestToSpot(request.get().spotId(), "again")
                            .instanceSpot()
                            .inMesh("game")
                            .timeout(Duration.ofSeconds(5))
                            .submit(String.class)
                            .thenApply(secondValue -> firstValue + "|"
                                + secondValue);
                    });
                CompletionStage<String> beforeAfterClose =
                    firstAndSecond.thenCompose(value -> {
                        return context.outbound()
                            .sendToSpot(
                                request.get().spotId(),
                                new CloseInstance())
                            .instanceSpot()
                            .inMesh("game")
                            .submit()
                            .thenApply(ignoredClose -> value);
                    });
                CompletableFuture<String> completion =
                    beforeAfterClose.thenCompose(value ->
                        context.outbound()
                            .requestToSpot(
                                request.get().spotId(),
                                "after-close")
                            .instanceSpot()
                            .inMesh("game")
                            .timeout(Duration.ofSeconds(5))
                            .submit(String.class)
                            .thenApply(after -> value + "|" + after))
                        .toCompletableFuture();
                completion.whenComplete((value, failure) -> {
                    if (failure == null) {
                        reply.complete(value);
                    } else {
                        reply.completeExceptionally(failure);
                    }
                });
                return completion.thenApply(value -> null);
            });
        }
    }

    public static final class IdleSourceEntrySpot
        implements ZLinkEntrySpot<ZLinkActor> {
        static CompletableFuture<Void> start;
        static java.util.concurrent.atomic.AtomicReference<Request> request;
        static CompletableFuture<String> reply;
        static ZLinkInMemoryLocationStore store;
        static String spotId;
        private final ZLinkEntrySpotContext context;

        public IdleSourceEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        static void reset() {
            start = new CompletableFuture<>();
            request = new java.util.concurrent.atomic.AtomicReference<>();
            reply = new CompletableFuture<>();
        }

        @Override public ZLinkEntrySpotContext context() { return context; }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return start.thenCompose(ignored ->
                context.outbound()
                    .requestToSpot(spotId, "hello")
                    .instanceSpot("EchoInstance")
                    .inMesh("game")
                    .timeout(Duration.ofSeconds(5))
                    .submit(String.class))
                .thenCompose(first -> {
                    long deadline = System.nanoTime()
                        + Duration.ofSeconds(5).toNanos();
                    return EchoInstanceSpot.idleEvicted
                        .thenCompose(ignored -> awaitAuthorityMissing(
                            store, spotId, deadline))
                        .thenComposeAsync(ignored -> context.outbound()
                            .requestToSpot(spotId, "ordinary-after-idle")
                            .inMesh("game")
                            .timeout(Duration.ofSeconds(5))
                            .submit(String.class)
                            .handle((value, failure) -> {
                                if (failure == null) {
                                    throw new AssertionError(
                                        "ordinary request succeeded after idle eviction");
                                }
                                Throwable cause = unwrap(failure);
                                assertTrue(cause instanceof ZLinkFrameworkException,
                                    "ordinary request failure was not typed: "
                                        + cause);
                                assertEquals(
                                    ZLinkFrameworkErrorKind.NOT_FOUND,
                                    ((ZLinkFrameworkException) cause).kind());
                                return (Void) null;
                            }),
                            CompletableFuture.delayedExecutor(
                                1, TimeUnit.MILLISECONDS))
                        .thenComposeAsync(ignored -> context.outbound()
                                .requestToSpot(spotId, "after-idle")
                                .instanceSpot("EchoInstance")
                                .inMesh("game")
                                .timeout(Duration.ofSeconds(5))
                                .submit(String.class),
                            CompletableFuture.delayedExecutor(
                                1, TimeUnit.MILLISECONDS))
                        .thenApply(after -> first + "|" + after);
                })
                .whenComplete((value, failure) -> {
                    if (failure == null) {
                        reply.complete(value);
                    } else {
                        reply.completeExceptionally(failure);
                    }
                })
                .thenApply(ignored -> null);
        }
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable cause = failure;
        while (cause instanceof CompletionException
            && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
    }

    private static CompletionStage<Void> awaitAuthorityMissing(
        ZLinkInMemoryLocationStore store,
        String spotId,
        long deadlineNanos) {
        return store.read(
                systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec
                    .spot(spotId),
                () -> false)
            .thenCompose(read -> {
                if (read instanceof systems.zlink.framework.runtime.internal.locations
                        .ZLinkAuthorityMissing) {
                    return CompletableFuture.completedFuture(null);
                }
                if (System.nanoTime() >= deadlineNanos) {
                    return CompletableFuture.failedFuture(
                        new AssertionError(
                            "Instance Spot authority was not deleted"));
                }
                return CompletableFuture.supplyAsync(
                        () -> (Void) null,
                        CompletableFuture.delayedExecutor(
                            10, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> awaitAuthorityMissing(
                        store, spotId, deadlineNanos));
            });
    }

    private static String tcpEndpoint() throws IOException {
        try (ServerSocket socket = new ServerSocket(0)) {
            return "tcp://127.0.0.1:" + socket.getLocalPort();
        }
    }

    public static final class EchoInstanceSpot implements ZLinkInstanceSpot {
        static final AtomicInteger initializations = new AtomicInteger();
        static final AtomicInteger sends = new AtomicInteger();
        static final java.util.concurrent.atomic.AtomicReference<Boolean> closes =
            new java.util.concurrent.atomic.AtomicReference<>();
        static final java.util.concurrent.atomic.AtomicReference<ZLinkSpotCloseReason>
            closeReason = new java.util.concurrent.atomic.AtomicReference<>();
        static volatile CompletableFuture<Void> idleEvicted =
            new CompletableFuture<>();
        private final ZLinkInstanceSpotContext context;

        public EchoInstanceSpot(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override public ZLinkInstanceSpotContext context() { return context; }

        @Override
        public void configure() {
            context.handlers().addPacket(EchoHandler.class);
            context.handlers().addPacket(EchoPacketHandler.class);
            context.handlers().addPacket(CloseHandler.class);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            initializations.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onClosing(
            systems.zlink.framework.spots.ZLinkSpotClosingContext closing) {
            closeReason.set(closing.reason());
            if (closing.reason() == ZLinkSpotCloseReason.IDLE_EVICTED) {
                idleEvicted.complete(null);
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EchoHandler
        implements ZLinkSpotRequestHandler<EchoInstanceSpot, String, String> {
        @Override
        public CompletionStage<String> handle(
            EchoInstanceSpot spot,
            String request) {
            return CompletableFuture.completedFuture("echo:" + request);
        }
    }

    public static final class EchoPacketHandler
        implements ZLinkSpotPacketHandler<EchoInstanceSpot, Warmup> {
        @Override
        public CompletionStage<Void> handle(
            EchoInstanceSpot spot,
            Warmup request) {
            EchoInstanceSpot.sends.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class CloseHandler
        implements ZLinkSpotPacketHandler<EchoInstanceSpot, CloseInstance> {
        @Override
        public CompletionStage<Void> handle(
            EchoInstanceSpot spot,
            CloseInstance request) {
            return spot.context().close().thenApply(closed -> {
                EchoInstanceSpot.closes.set(closed);
                return null;
            });
        }
    }
}
