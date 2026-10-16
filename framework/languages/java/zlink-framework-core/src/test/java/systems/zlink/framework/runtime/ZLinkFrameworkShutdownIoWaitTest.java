package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.*;

/** Host relocation §14: owned I/O waits end before closing and same-store replacement. */
final class ZLinkFrameworkShutdownIoWaitTest {
    @TempDir Path logs;
    private static final int WAIT_SECONDS = 10;
    private static final int SHUTDOWN_WATCHDOG_SECONDS = 2;
    private static final Duration SHUTDOWN_DEADLINE = Duration.ofMillis(250);
    private static final String MESH_NAME = "shutdown-io-wait";
    private static final String SPOT_ID = "shutdown-room";
    private static final String FACTORY_NAME = "probe";
    private static CompletableFuture<Void> entered;
    private static AtomicInteger handlerTerminals;
    private static CompletableFuture<String> io;
    private static AtomicInteger closing;

    public record Probe(boolean hold) {}

    public record Reply(String value) {}

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void shutdownEndsOwnedIoWaitAndAllowsSameStoreReplacement(boolean finishBeforeShutdown)
            throws Exception {
        entered = new CompletableFuture<>();
        handlerTerminals = new AtomicInteger();
        io = new CompletableFuture<>();
        closing = new AtomicInteger();
        String label = finishBeforeShutdown ? "control" : "pending";
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        String flowDirectory = System.getenv("ZLINK_SHUTDOWN_TEST_FLOW_DIR");
        Path flowPath = flowDirectory == null ? logs : Path.of(flowDirectory);
        var log = new FileHandler(flowPath.resolve(label + ".flow").toString());
        log.setFormatter(new SimpleFormatter());
        logger.addHandler(log);
        var store = new ZLinkInMemoryLocationStore();
        var recovery = new InMemoryRelocationStore();
        var options = options(store, recovery);
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
        CompletableFuture<ZLinkFrameworkTerminationResult> shutdown = null;
        boolean bounded = true;
        try {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var request =
                    runtime.route()
                            .requestToSpot(SPOT_ID, new Probe(true))
                            .instanceSpot(FACTORY_NAME)
                            .inMesh(MESH_NAME)
                            .timeout(Duration.ofSeconds(WAIT_SECONDS))
                            .submit(Reply.class)
                            .toCompletableFuture();
            entered.get(WAIT_SECONDS, TimeUnit.SECONDS);
            if (finishBeforeShutdown) {
                io.complete("ok");
                assertEquals(new Reply("ok"), request.get(WAIT_SECONDS, TimeUnit.SECONDS));
            }
            shutdown = runtime.shutdown(SHUTDOWN_DEADLINE).toCompletableFuture();
            try {
                shutdown.get(SHUTDOWN_WATCHDOG_SECONDS, TimeUnit.SECONDS);
            } catch (TimeoutException timeout) {
                bounded = false;
            }
            assertTrue(bounded, "shutdown must finish while the original I/O remains pending");
            assertEquals(1, closing.get());
            assertEquals(1, handlerTerminals.get());
            assertEquals(finishBeforeShutdown, io.isDone());
        } finally {
            io.complete("late");
            try {
                if (shutdown != null) {
                    shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    assertEquals(1, closing.get());
                    assertEquals(1, handlerTerminals.get());
                    try (var replacement =
                            ZLinkFrameworkRuntimeTestAccess.start(options(store, recovery))) {
                        ZLinkFrameworkRuntimeTestAccess.startupCompletion(replacement)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        var reply =
                                replacement
                                        .route()
                                        .requestToSpot(SPOT_ID, new Probe(false))
                                        .instanceSpot(FACTORY_NAME)
                                        .inMesh(MESH_NAME)
                                        .timeout(Duration.ofSeconds(WAIT_SECONDS))
                                        .submit(Reply.class)
                                        .toCompletableFuture()
                                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        assertEquals(new Reply("ok"), reply);
                    }
                } else runtime.close();
            } finally {
                logger.removeHandler(log);
                log.close();
            }
        }
        assertEquals(
                finishBeforeShutdown
                        ? ZLinkFrameworkTerminationOutcome.STOPPED
                        : ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
                shutdown.join().outcome());
        assertEquals(
                finishBeforeShutdown
                        ? ZLinkFrameworkTerminationReason.NONE
                        : ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED,
                shutdown.join().reason());
    }

    private static DefaultZLinkFrameworkOptions options(
            ZLinkInMemoryLocationStore store, InMemoryRelocationStore recovery) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(recovery);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh(MESH_NAME)
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addInstanceSpotFactory(FACTORY_NAME, Instance.class, f -> f.disableRelocation());
        return options;
    }

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(Handler.class);
        }

        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext context) {
            assertEquals(1, handlerTerminals.get());
            closing.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Handler implements ZLinkSpotRequestHandler<Instance, Probe, Reply> {
        public CompletionStage<Reply> handle(Instance spot, Probe request) {
            if (!request.hold()) return CompletableFuture.completedFuture(new Reply("ok"));
            var waiting = spot.context().runIoWorker(cancellation -> io).yield();
            entered.complete(null);
            return waiting.thenApply(Reply::new)
                    .whenComplete((reply, failure) -> handlerTerminals.incrementAndGet());
        }
    }
}
