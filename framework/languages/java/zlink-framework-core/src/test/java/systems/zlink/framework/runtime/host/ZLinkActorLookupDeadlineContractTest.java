package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.*;

/** Actor request §9 and message-flow §2.2, through public calls and the opaque Store SPI. */
final class ZLinkActorLookupDeadlineContractTest {
    static final AtomicInteger handled = new AtomicInteger();

    public record Ping(String value) {}

    public record Pong(String value) {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        public void configure() {
            context.handlers().addHandler(PingHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PingHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<Pong> ping(
                Entry entry, Player actor, ZLinkMessageContext context, Ping request) {
            handled.incrementAndGet();
            return CompletableFuture.completedFuture(new Pong(request.value()));
        }
    }

    static final class HeldStore implements ZLinkLocationStore {
        final ZLinkLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();
        final CompletableFuture<Void> returned = new CompletableFuture<>();
        volatile boolean hold;

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (hold && key.value().equals("authority\0actor\0deadline-player")) {
                entered.complete(null);
                return release.thenCompose(ignored -> inner.read(key, cancellation))
                        .whenComplete((value, failure) -> returned.complete(null));
            }
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    @Test
    void deadlineIncludesAuthorityReadAndCallerTerminalIsRecordedOnce() throws Exception {
        var root = java.nio.file.Path.of(System.getProperty("user.dir")).toAbsolutePath();
        var relative =
                java.nio.file.Path.of("runtime/conformance/actor-request-lookup-deadline-v1.json");
        while (root != null && !java.nio.file.Files.isRegularFile(root.resolve(relative)))
            root = root.getParent();
        assertNotNull(root);
        var fixture =
                new com.fasterxml.jackson.databind.ObjectMapper()
                        .readTree(java.nio.file.Files.readString(root.resolve(relative)));
        var store = new HeldStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.DETAILED);
        options.addRouteMesh("deadline")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory("player", Player.class, Factory.class, f -> f.disableRelocation());
        var records = new CopyOnWriteArrayList<String>();
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var listener =
                new Handler() {
                    public void publish(LogRecord record) {
                        records.add(record.getMessage());
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(listener);
        try (var host = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            host.actorManager()
                    .getOrCreate("deadline-player", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            assertEquals("warmup", request(host, "warmup", 3000).get(5, TimeUnit.SECONDS).value());
            handled.set(0);
            ZLinkTelemetryTestFence.awaitIdle();
            records.clear();
            store.hold = true;
            var call = request(host, "held", fixture.get("timeoutMs").asLong());
            try {
                store.entered.get(5, TimeUnit.SECONDS);
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () ->
                                        call.get(
                                                fixture.get("observationMs").asLong(),
                                                TimeUnit.MILLISECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
                assertFalse(store.release.isDone());
            } finally {
                store.release.complete(null);
            }
            store.returned.get(5, TimeUnit.SECONDS);
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(
                    fixture.get("lateHandlerCalls").asInt(),
                    handled.get(),
                    "terminal request must not reach its handler after lookup completes");
            var terminal =
                    records.stream()
                            .filter(
                                    line ->
                                            line.contains("phase=completed")
                                                    && line.contains("actor=deadline-player")
                                                    && line.contains("outcome=failed"))
                            .toList();
            assertEquals(1, terminal.size(), records.toString());
            assertTrue(terminal.getFirst().contains("ZLinkFrameworkException"));
            store.hold = false;
            assertEquals("after", request(host, "after", 3000).get(5, TimeUnit.SECONDS).value());
            assertEquals(1, handled.get());
        } finally {
            store.release.complete(null);
            ZLinkTelemetryTestFence.awaitIdle();
            logger.removeHandler(listener);
        }
    }

    private static CompletableFuture<Pong> request(
            ZLinkFrameworkRuntime host, String value, long timeout) {
        return host.actorClient()
                .requestToActor("deadline-player", new Ping(value))
                .timeout(Duration.ofMillis(timeout))
                .submit(Pong.class)
                .toCompletableFuture();
    }
}
