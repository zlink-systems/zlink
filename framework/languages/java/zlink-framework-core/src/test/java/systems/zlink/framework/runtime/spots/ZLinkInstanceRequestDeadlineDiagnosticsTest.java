package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNodeTestAccess;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLaneTestAccess;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistryTestAccess;
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
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.LongAccumulator;
import java.util.function.Supplier;

class ZLinkInstanceRequestDeadlineDiagnosticsTest {
    private static final String FLAG = "ZLINK_DEBUG_FRAMEWORK_RELOCATION";
    private static final String MARKER = "[zlink.runtime.relocation] node_request_deadline";
    private static final String CALLER_MARKER = "deadline callerFutureCompletedNanos: ";
    private static final int COMPLETION_WORKERS = 2;
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(2);
    // Covers thread wake-up and clock-read jitter after the measured occupation ends; it is not
    // a spec deadline (01-submit-and-completion.ko.md §10.1 allows the late visit itself).
    private static final long SCHEDULING_SLACK_NANOS = TimeUnit.MILLISECONDS.toNanos(250);
    private static final long HOLD_PAST_DEADLINE_NANOS = 2 * SCHEDULING_SLACK_NANOS;
    private static final Duration POLL_INTERVAL = Duration.ofMillis(1);
    private static final String MESH = "deadline-mesh";
    private static final String TARGET = "deadline-owner";
    private static final CompletableFuture<HandlerEntry> ENTERED = new CompletableFuture<>();
    private static final CompletableFuture<Echo> BLOCKED = new CompletableFuture<>();

    @TempDir Path outputDir;

    @Test
    void boundsDeadlineWithIdleLaneAndOccupiedExecutionResources() throws Exception {
        assertDeadlineReport(run("1"));
        assertDeadlineReport(run("1", "owner-delay", "-ea"));
        assertDeadlineReport(run("1", "completion-delay", "-ea"));
    }

    private static void assertDeadlineReport(String output) {
        System.out.print(output);
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
        long scheduled = timestamp(output, "scheduledDeadlineNanos: ");
        long ownerVisit = timestamp(output, "ownerVisitNanos: ");
        long terminal = timestamp(output, "terminalNanos: ");
        long dispatcher = timestamp(output, "dispatcherStartNanos: ");
        long caller = timestamp(output, CALLER_MARKER);
        assertTrue(scheduled <= ownerVisit && ownerVisit <= terminal, output);
        assertTrue(terminal <= dispatcher && dispatcher <= caller, output);
    }

    @Test
    void omitsTheExpiredRequestWithDiagnosticsDisabled() throws Exception {
        String output = run("0");
        assertFalse(output.contains(MARKER), output);
        assertFalse(output.contains(CALLER_MARKER), output);
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

    private static long timestamp(String output, String label) {
        var matcher = java.util.regex.Pattern.compile(label + "(-?[0-9]+)").matcher(output);
        assertTrue(matcher.find(), output);
        return Long.parseLong(matcher.group(1));
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
                                mode,
                                output.toString())
                        .redirectErrorStream(true)
                        .redirectOutput(output.toFile());
        if (mode.equals("completion-delay")) {
            process.command()
                    .add(
                            1,
                            "-Djava.util.concurrent.ForkJoinPool.common.parallelism="
                                    + COMPLETION_WORKERS);
        }
        process.environment().put(FLAG, flag);
        var child = process.start();
        try {
            if (!child.waitFor(20, TimeUnit.SECONDS)) {
                fail("child timed out: " + output + "\n" + Files.readString(output));
            }
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
                            .timeout(REQUEST_TIMEOUT)
                            .submit(Echo.class)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            var spots = ZLinkFrameworkRuntimeTestAccess.spots(runtime);
            var registry =
                    ZLinkJavaRawMeshNodeTestAccess.operationRegistry(
                            spots.routeMeshNodesByName.get(MESH));
            try {
                var pending =
                        runtime.route()
                                .requestToSpot("room-1", new Echo(true))
                                .instanceSpot("room")
                                .inMesh(MESH)
                                .timeout(REQUEST_TIMEOUT)
                                .submit(Echo.class)
                                .toCompletableFuture();
                observeDeadline(args[0], Path.of(args[1]), registry, pending);
            } finally {
                BLOCKED.complete(new Echo(true));
            }
        }
    }

    private static void observeDeadline(
            String mode,
            Path output,
            ZLinkServiceOperationRegistry registry,
            CompletableFuture<Echo> pending)
            throws Exception {
        var lane = ZLinkServiceOperationRegistryTestAccess.ownerLane(registry);
        var pool = ForkJoinPool.commonPool();
        var callerCompletion = pending.handle((reply, failure) -> System.nanoTime());
        boolean holdOwner = mode.equals("owner-delay");
        boolean holdPool = mode.equals("completion-delay");
        var blocker = new Blocker(holdPool ? COMPLETION_WORKERS : 1);
        try {
            ENTERED.get(5, TimeUnit.SECONDS);
            var id = ZLinkServiceOperationRegistryTestAccess.onlyPendingId(registry);
            var remaining = registry.remainingTimeout(id);
            long deadlineUpper = System.nanoTime() + remaining.toNanos();
            assertFalse(remaining.isZero(), "request expired before fixture setup");
            if (holdOwner) {
                assertTrue(
                        lane.tryPost(
                                () -> {
                                    blocker.run();
                                    return CompletableFuture.completedFuture(null);
                                }));
            } else if (holdPool) {
                assertEquals(COMPLETION_WORKERS, pool.getParallelism());
                for (int worker = 0; worker < COMPLETION_WORKERS; worker++) {
                    pool.execute(blocker::run);
                }
            }

            var occupation = Occupation.NONE;
            if (holdOwner || holdPool) {
                assertTrue(blocker.entered.await(5, TimeUnit.SECONDS), mode);
                assertTrue(blocker.started.get() < deadlineUpper, "fixture missed the deadline");
                long holdUntil = deadlineUpper + HOLD_PAST_DEADLINE_NANOS;
                if (holdPool) awaitTerminal(registry, upperBound(deadlineUpper, Occupation.NONE));
                for (long delay; (delay = holdUntil - System.nanoTime()) > 0; ) {
                    TimeUnit.NANOSECONDS.sleep(delay);
                }
                assertEquals(holdOwner ? 1 : 0, registry.pendingCount(), mode);
                assertFalse(pending.isDone(), mode);
                occupation = blocker.stop();
            }

            var ownerWork = holdOwner ? occupation : Occupation.NONE;
            var poolWork = holdPool ? occupation : Occupation.NONE;
            long terminalObserved = awaitTerminal(registry, upperBound(deadlineUpper, ownerWork));
            long caller =
                    callerCompletion.get(
                            Math.max(0, upperBound(terminalObserved, poolWork) - System.nanoTime()),
                            TimeUnit.NANOSECONDS);
            var failure =
                    assertThrows(
                            ExecutionException.class, () -> pending.get(0, TimeUnit.NANOSECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            if ("1".equals(System.getenv(FLAG))) {
                String log = Files.readString(output);
                long terminal = timestamp(log, "terminalNanos: ");
                long dispatcher = timestamp(log, "dispatcherStartNanos: ");
                // The wheel rounds up. The recorded tick gives the lower bound for expiry,
                // and the registry's unrounded deadline gives the upper bound.
                long scheduled = timestamp(log, "scheduledDeadlineNanos: ");
                assertTrue(scheduled <= terminal, log);
                assertBy("terminal claim", terminal, deadlineUpper, ownerWork);
                assertBy("caller delivery", caller, terminal, poolWork);
                if (holdOwner) assertTrue(ownerWork.end() <= terminal, log);
                if (holdPool) {
                    assertTrue(terminal <= poolWork.end() && poolWork.end() <= dispatcher, log);
                }
                System.err.printf(
                        "deadline mode=%s deadlineUpperNanos=%d ownerWaitNanos=%d"
                                + " poolWaitNanos=%d slackNanos=%d%n",
                        mode,
                        deadlineUpper,
                        ownerWork.after(deadlineUpper),
                        poolWork.after(terminal),
                        SCHEDULING_SLACK_NANOS);
                System.err.println(CALLER_MARKER + caller);
            }
        } catch (AssertionError | Exception failure) {
            long observed = System.nanoTime();
            String ownerWork = ZLinkStateLaneTestAccess.describeActiveTurn(lane);
            var handler = ENTERED.getNow(null);
            long fixtureStart = blocker.started.get();
            long fixtureEnd = blocker.ended.get();
            System.err.printf(
                    "deadline failure mode=%s observedNanos=%d %s ownerQueuedTurns=%d"
                            + " pendingRequests=%d handlerKind=application handlerStartedNanos=%s"
                            + " handlerEnteredOnOwner=%s commonPoolActiveThreads=%d"
                            + " commonPoolQueuedTasks=%d commonPoolQueuedSubmissions=%d"
                            + " fixtureStartedNanos=%s fixtureEndedNanos=%s%n",
                    mode,
                    observed,
                    ownerWork,
                    ZLinkStateLaneTestAccess.queuedTurnCount(lane),
                    registry.pendingCount(),
                    handler == null ? "none" : Long.toString(handler.startedNanos()),
                    handler == null ? "none" : Boolean.toString(handler.lane() == lane),
                    pool.getActiveThreadCount(),
                    pool.getQueuedTaskCount(),
                    pool.getQueuedSubmissionCount(),
                    fixtureStart == Long.MIN_VALUE ? "none" : Long.toString(fixtureStart),
                    fixtureEnd == Long.MAX_VALUE ? "none" : Long.toString(fixtureEnd));
            throw failure;
        } finally {
            blocker.release.countDown();
        }
    }

    private static long awaitTerminal(ZLinkServiceOperationRegistry registry, long bound)
            throws InterruptedException {
        while (registry.pendingCount() != 0) {
            long left = bound - System.nanoTime();
            assertTrue(left > 0, () -> "terminal claim exceeded boundNanos=" + bound);
            TimeUnit.NANOSECONDS.sleep(Math.min(left, POLL_INTERVAL.toNanos()));
        }
        return System.nanoTime();
    }

    private static long upperBound(long start, Occupation occupation) {
        return start + occupation.after(start) + SCHEDULING_SLACK_NANOS;
    }

    private static void assertBy(String phase, long actual, long start, Occupation occupation) {
        long bound = upperBound(start, occupation);
        assertTrue(
                actual <= bound,
                () ->
                        phase
                                + " at "
                                + actual
                                + " exceeded boundNanos="
                                + bound
                                + " startNanos="
                                + start
                                + " measuredWaitNanos="
                                + occupation.after(start)
                                + " slackNanos="
                                + SCHEDULING_SLACK_NANOS);
    }

    private record Occupation(long start, long end) {
        static final Occupation NONE = new Occupation(0, 0);

        long after(long time) {
            return Math.max(0, end - Math.max(time, start));
        }
    }

    private static final class Blocker {
        private final CountDownLatch entered;
        private final CountDownLatch release = new CountDownLatch(1);
        private final CountDownLatch exited;
        private final LongAccumulator started = new LongAccumulator(Long::max, Long.MIN_VALUE);
        private final LongAccumulator ended = new LongAccumulator(Long::min, Long.MAX_VALUE);

        Blocker(int workers) {
            entered = new CountDownLatch(workers);
            exited = new CountDownLatch(workers);
        }

        void run() {
            started.accumulate(System.nanoTime());
            entered.countDown();
            try {
                assertTrue(release.await(10, TimeUnit.SECONDS), "test did not release fixture");
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                throw new AssertionError(failure);
            } finally {
                ended.accumulate(System.nanoTime());
                exited.countDown();
            }
        }

        Occupation stop() throws InterruptedException {
            release.countDown();
            assertTrue(exited.await(5, TimeUnit.SECONDS), "fixture did not exit");
            return new Occupation(started.get(), ended.get());
        }
    }

    private record HandlerEntry(long startedNanos, ZLinkStateLane lane) {}

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
            ENTERED.complete(new HandlerEntry(System.nanoTime(), ZLinkStateLane.current()));
            return BLOCKED;
        }
    }
}
