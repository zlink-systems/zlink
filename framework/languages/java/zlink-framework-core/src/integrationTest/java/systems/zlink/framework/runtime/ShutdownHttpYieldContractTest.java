package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;
import systems.zlink.httpclient.TestSupport;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class ShutdownHttpYieldContractTest {
    @Test
    void shutdownTerminatesHttpYieldBeforeClosingAndReleasesInstanceAuthority() throws Exception {
        Probe.completed = new CompletableFuture<>();
        Probe.http = new CompletableFuture<>();
        Probe.closingFailure = null;
        Probe.terminals.set(0);
        var errors = new java.util.concurrent.CopyOnWriteArrayList<String>();
        var logger =
                java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        var listener =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        errors.add(record.getMessage());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(listener);
        var entered = new CompletableFuture<Void>();
        var release = new CountDownLatch(1);
        var server =
                TestSupport.httpServer(
                        exchange -> {
                            entered.complete(null);
                            try {
                                release.await();
                                byte[] body =
                                        "response"
                                                .getBytes(java.nio.charset.StandardCharsets.UTF_8);
                                exchange.sendResponseHeaders(200, body.length);
                                exchange.getResponseBody().write(body);
                            } catch (InterruptedException interrupted) {
                                Thread.currentThread().interrupt();
                            } finally {
                                exchange.close();
                            }
                        });
        Probe.uri = URI.create(server.baseUrl() + "/generate");
        var store = new ZLinkInMemoryLocationStore();
        String id = "shutdown-http-" + Long.toUnsignedString(System.nanoTime());
        try (var source = start(store, id + "-source", false)) {
            var target = start(store, id + "-target", true);
            awaitPeers(source, target);
            var request =
                    Source.context
                            .outbound()
                            .requestToSpot(id, "hold")
                            .instanceSpot("probe")
                            .inMesh("game")
                            .submit(String.class);
            request.exceptionally(error -> null);
            entered.get(5, TimeUnit.SECONDS);
            var result =
                    target.shutdown(Duration.ofMillis(250))
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
            assertAll(
                    () ->
                            assertEquals(
                                    ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
                                    result.outcome()),
                    () ->
                            assertEquals(
                                    ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED,
                                    result.reason()),
                    () ->
                            assertTrue(
                                    Probe.completed.isDone(),
                                    "runtime wait must terminate before scope cleanup"));
            int terminals = Probe.terminals.get();
            release.countDown();
            assertEquals("response", Probe.http.get(5, TimeUnit.SECONDS));
            assertEquals(
                    terminals,
                    Probe.terminals.get(),
                    "late HTTP completion must not resume the handler");
            assertFalse(
                    errors.stream()
                            .anyMatch(message -> message.contains("reason=handler_exception")),
                    errors.toString());
            try (var replacement = start(store, id + "-replacement", true)) {
                awaitPeers(source, replacement);
                assertEquals(
                        "echo",
                        Source.context
                                .outbound()
                                .requestToSpot(id, "echo")
                                .instanceSpot("probe")
                                .inMesh("game")
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS));
            }
        } finally {
            release.countDown();
            server.closeable().close();
            logger.removeHandler(listener);
        }
    }

    @Test
    void callbackTimeoutExceptionIsTeardownFailure() throws Exception {
        Probe.closingFailure = new TimeoutException("application closing callback failed");
        var store = new ZLinkInMemoryLocationStore();
        String id = "shutdown-callback-" + Long.toUnsignedString(System.nanoTime());
        try (var source = start(store, id + "-source", false)) {
            var target = start(store, id + "-target", true);
            awaitPeers(source, target);
            assertEquals(
                    "echo",
                    Source.context
                            .outbound()
                            .requestToSpot(id, "echo")
                            .instanceSpot("probe")
                            .inMesh("game")
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            var result =
                    target.shutdown(Duration.ofSeconds(2))
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
            assertEquals(ZLinkFrameworkTerminationReason.TEARDOWN_FAILED, result.reason());
        } finally {
            Probe.closingFailure = null;
        }
    }

    private static ZLinkFrameworkRuntime start(
            ZLinkInMemoryLocationStore store, String rid, boolean target) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var mesh =
                options.addRouteMesh("game")
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (target)
            mesh.objects()
                    .server()
                    .addInstanceSpotFactory(
                            "probe", Probe.class, factory -> factory.disableRelocation());
        else {
            mesh.objects().client();
            mesh.objects().server().addEntrySpot(Source.class);
        }
        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static void awaitPeers(ZLinkFrameworkRuntime source, ZLinkFrameworkRuntime target)
            throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!source.isReady()
                || !target.isReady()
                || source.routeMeshRuntime().snapshot("game").peers().stream()
                        .noneMatch(
                                peer ->
                                        peer.state()
                                                == systems.zlink.framework.monitoring.ZLinkPeerState
                                                        .READY)) {
            if (System.nanoTime() >= end) throw new TimeoutException("hosts never became ready");
            Thread.sleep(2);
        }
    }

    public static final class Source implements ZLinkEntrySpot<ZLinkActor> {
        static ZLinkEntrySpotContext context;

        public Source(ZLinkEntrySpotContext value) {
            context = value;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Probe implements ZLinkInstanceSpot {
        static URI uri;
        static Throwable closingFailure;
        static CompletableFuture<Void> completed;
        static CompletableFuture<String> http;
        static final AtomicInteger terminals = new AtomicInteger();
        private final ZLinkInstanceSpotContext context;

        public Probe(ZLinkInstanceSpotContext value) {
            context = value;
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(Handler.class);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            return closingFailure == null
                    ? CompletableFuture.completedFuture(null)
                    : CompletableFuture.failedFuture(closingFailure);
        }
    }

    public static final class Handler implements ZLinkSpotRequestHandler<Probe, String, String> {
        @Override
        public CompletionStage<String> handle(Probe spot, String message) {
            if (!message.equals("hold")) return CompletableFuture.completedFuture(message);
            return spot.context()
                    .runIoWorker(
                            cancellation -> {
                                var pending =
                                        HttpClient.newHttpClient()
                                                .sendAsync(
                                                        HttpRequest.newBuilder(Probe.uri).build(),
                                                        HttpResponse.BodyHandlers.ofString())
                                                .thenApply(HttpResponse::body);
                                pending.whenComplete(
                                        (value, error) -> {
                                            if (error == null) Probe.http.complete(value);
                                            else Probe.http.completeExceptionally(error);
                                        });
                                return pending;
                            })
                    .yield()
                    .whenComplete(
                            (value, error) -> {
                                Probe.terminals.incrementAndGet();
                                Probe.completed.complete(null);
                            });
        }
    }
}
