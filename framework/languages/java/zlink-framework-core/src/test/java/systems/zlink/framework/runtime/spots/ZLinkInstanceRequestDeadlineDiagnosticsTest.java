package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRequestProgress.InstanceReply;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.testutil.PerCallAllocation;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;

class ZLinkInstanceRequestDeadlineDiagnosticsTest {
    private static final String FLAG = "ZLINK_DEBUG_FRAMEWORK_RELOCATION";
    private static final String MARKER = "[zlink.runtime.relocation] node_request_deadline";
    private static final String MESH = "deadline-mesh";
    private static final String TARGET = "deadline-owner";
    private static final CompletableFuture<Void> ENTERED = new CompletableFuture<>();
    private static final CompletableFuture<Echo> BLOCKED = new CompletableFuture<>();

    @TempDir Path outputDir;

    @Test
    void reportsOnlyTheExpiredRequestWithDiagnosticsEnabled() throws Exception {
        String output = run("1");
        assertEquals(1, output.lines().filter(line -> line.startsWith(MARKER)).count(), output);
        assertTrue(output.contains("meshName: 'deadline-mesh'"), output);
        assertTrue(output.contains("targetRid: 'deadline-owner'"), output);
        assertTrue(output.contains("stage: 'reply_wait'"), output);
        assertTrue(output.matches("(?s).*correlationId: '[1-9][0-9]*'.*"), output);
        assertTrue(output.matches("(?s).*routeGeneration: [1-9][0-9]*n.*"), output);
        double selected = elapsed(output, "routeSelectedMs");
        double admitted = elapsed(output, "peerAdmittedMs");
        double dispatched = elapsed(output, "localDispatchMs");
        assertTrue(0 <= selected && selected <= admitted && admitted <= dispatched, output);
        assertTrue(dispatched <= elapsed(output, "elapsedMs"), output);
        assertTrue(output.contains("bindingSubmittedMs: undefined"), output);
        assertTrue(output.contains("bindingAdmittedMs: undefined"), output);
        assertTrue(output.contains("replyReceivedMs: undefined"), output);
    }

    @Test
    void omitsTheExpiredRequestWithDiagnosticsDisabled() throws Exception {
        String output = run("0");
        assertFalse(output.contains(MARKER), output);
    }

    @Test
    void disabledProgressAllocatesNoAdditionalBytes() throws Exception {
        String disabled = run("0", "allocations", "-XX:-DoEscapeAnalysis");
        String enabled = run("1", "allocations", "-XX:-DoEscapeAnalysis");
        assertTrue(disabled.contains("added="), disabled);
        System.out.print(disabled);
        System.out.print(enabled);
    }

    private static double elapsed(String output, String field) {
        var matcher =
                java.util.regex.Pattern.compile(field + ": ([0-9]+(?:[.][0-9]+)?(?:E-?[0-9]+)?)")
                        .matcher(output);
        assertTrue(matcher.find(), output);
        return Double.parseDouble(matcher.group(1));
    }

    private String run(String flag) throws Exception {
        return run(flag, "request", "-ea");
    }

    private String run(String flag, String mode, String vmFlag) throws Exception {
        Path output = outputDir.resolve(mode + "-" + flag + ".log");
        var process =
                new ProcessBuilder(
                                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                vmFlag,
                                "-cp",
                                System.getProperty("java.class.path"),
                                getClass().getName(),
                                mode)
                        .redirectErrorStream(true)
                        .redirectOutput(output.toFile());
        process.environment().put(FLAG, flag);
        var child = process.start();
        try {
            assertTrue(child.waitFor(20, TimeUnit.SECONDS), () -> "child timed out: " + output);
            String log = Files.readString(output);
            assertEquals(0, child.exitValue(), log);
            return log;
        } finally {
            if (child.isAlive()) {
                child.destroyForcibly();
                child.waitFor();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("allocations")) {
            measureAllocations();
            return;
        }
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addRouteMesh(MESH)
                .listen("inproc://deadline-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from(TARGET))
                .objects()
                .server()
                .addInstanceSpotFactory("room", Room.class, factory -> factory.disableRelocation());

        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            assertTrue(
                    ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertTrue(runtime.isReady());
            assertEquals(
                    new Echo(false),
                    runtime.route()
                            .requestToSpot("room-1", new Echo(false))
                            .instanceSpot("room")
                            .inMesh(MESH)
                            .timeout(Duration.ofSeconds(2))
                            .submit(Echo.class)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            try {
                var pending =
                        runtime.route()
                                .requestToSpot("room-1", new Echo(true))
                                .instanceSpot("room")
                                .inMesh(MESH)
                                .timeout(Duration.ofSeconds(2))
                                .submit(Echo.class)
                                .toCompletableFuture();
                ENTERED.get(5, TimeUnit.SECONDS);
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> pending.get(5, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            } finally {
                BLOCKED.complete(new Echo(true));
            }
        }
    }

    private static void measureAllocations() {
        var lane = new ZLinkStateLane();
        var rid = RoutingId.from(TARGET);
        var route =
                new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                        rid, 1, "room-1", MESH, "room", "version", 1);
        var header =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        0, route, true, 1, rid, "source", true, 1, 1, 1L);
        byte[] metadata = new byte[0];
        var timeout = Duration.ofSeconds(20);
        int calls = 1_000;
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        lane,
                        new IllegalStateException("closed"),
                        TimeoutException::new,
                        () -> 0L)) {
            var cost =
                    lane.awaitTurn(
                            () ->
                                    PerCallAllocation.minAlternatingWindow(
                                            new Supplier<>() {
                                                boolean observed;

                                                @Override
                                                public ZLinkBackendReceived get() {
                                                    var operation =
                                                            observed
                                                                    ? registry
                                                                            .registerInstanceRequestOnOwner(
                                                                                    timeout, MESH,
                                                                                    rid, 1, 1, true)
                                                                    : registry.registerOnOwner(
                                                                            timeout);
                                                    var progress = operation.requestProgress();
                                                    ZLinkBackendReceived received;
                                                    if (observed) {
                                                        var reply =
                                                                progress.localReply(
                                                                        registry, operation.id());
                                                        received =
                                                                reply.progress()
                                                                        .receiveInstance(
                                                                                rid, header,
                                                                                metadata, List.of(),
                                                                                reply, null);
                                                        received.requestProgress().peerAdmitted();
                                                        received.requestProgress().handlerStarted();
                                                        progress.bindingSubmitted();
                                                        progress.bindingAdmission(null);
                                                        progress.replyReceived();
                                                    } else {
                                                        InstanceReply reply =
                                                                parts ->
                                                                        registry.complete(
                                                                                operation.id(),
                                                                                parts);
                                                        received =
                                                                new ZLinkBackendReceived(
                                                                        ZLinkBackendRequestResult
                                                                                .OK,
                                                                        Optional.of(rid),
                                                                        Optional.of("source"),
                                                                        Optional.of(1L),
                                                                        metadata,
                                                                        new byte[0],
                                                                        List.of(),
                                                                        reply,
                                                                        null,
                                                                        null);
                                                    }
                                                    if (!registry.discard(operation.id()))
                                                        throw new AssertionError("discard");
                                                    observed = !observed;
                                                    return received;
                                                }
                                            },
                                            calls,
                                            10_000));
            long added = cost.measuredBytes() - cost.baselineBytes();
            System.out.printf(
                    "calls=%d baseline=%d measured=%d added=%d%n",
                    calls, cost.baselineBytes(), cost.measuredBytes(), added);
            if ("1".equals(System.getenv(FLAG))) assertTrue(added > 0, cost.toString());
            else {
                // Disabled diagnostics may add no bytes. The baseline can include one-off JVM
                // allocations, so only an increase over it is a cost.
                assertTrue(added <= 0, cost.toString());
            }
        }
    }

    public record Echo(boolean blocked) {}

    public static final class Room implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Room(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(Handler.class);
        }
    }

    public static final class Handler implements ZLinkSpotRequestHandler<Room, Echo, Echo> {
        public CompletionStage<Echo> handle(Room room, Echo echo) {
            if (!echo.blocked()) return CompletableFuture.completedFuture(echo);
            ENTERED.complete(null);
            return BLOCKED;
        }
    }
}
