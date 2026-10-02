package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.*;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkInstanceSpotCloseConformanceTest {
    private static final String MESH = "close-instance-mesh";
    private static final String TYPE = "close-instance";
    private static final Duration WAIT = Duration.ofSeconds(10);
    private static final Duration UNEXECUTED_TIMER_PERIOD = WAIT.multipliedBy(4);
    private static final String UNEXECUTED_TIMER_NAME = "close-unexecuted-timer";
    private static final String FLOW_LOGGER =
            "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer";
    static final Set<String> BRANCHES =
            Set.of(
                    "reincarnate-pending-intent",
                    "release-without-pending-intent",
                    "release-during-host-drain-or-relocation",
                    "reincarnate-initialization-fails",
                    "closing-message-without-intent");
    private static Observation current;

    @Test
    void pendingMessageAcceptedBeforeCloseRunsOnlyInTheNewIncarnation() throws Exception {
        run(null, "Serving", true, true, false);
    }

    @Test
    void readyInstanceIntentArrivingAtClosingOwnerRunsInTheNewIncarnation() throws Exception {
        run(null, "Serving", true, false, false);
    }

    @Test
    void reincarnationConfigureFailureDeletesGenerationAndFailsPendingRequest() throws Exception {
        run(null, "Serving", true, false, true, true);
    }

    static void runBranch(JsonNode branch) throws Exception {
        String name = branch.path("name").asText();
        assertTrue(BRANCHES.contains(name), "unknown close branch " + name);
        if (name.equals("release-during-host-drain-or-relocation")) {
            for (JsonNode host : branch.path("given").path("host")) {
                run(branch, host.asText(), true, false, false);
            }
        } else {
            boolean intent = branch.path("given").path("pendingIntent").asBoolean(false);
            boolean failing = name.equals("reincarnate-initialization-fails");
            run(branch, "Serving", intent, false, failing);
        }
    }

    private static void run(
            JsonNode branch,
            String host,
            boolean intent,
            boolean queuedBeforeClose,
            boolean failInitialization)
            throws Exception {
        run(branch, host, intent, queuedBeforeClose, failInitialization, false);
    }

    private static void run(
            JsonNode branch,
            String host,
            boolean intent,
            boolean queuedBeforeClose,
            boolean failInitialization,
            boolean failConfigure)
            throws Exception {
        String spotId = "java-close-instance-" + UUID.randomUUID();
        Observation observation = new Observation(failInitialization, queuedBeforeClose);
        observation.failConfigure = failConfigure;
        current = observation;
        ObservedStore store = new ObservedStore(spotId, observation);
        ZLinkProviderLocationRepository repository = new ZLinkProviderLocationRepository(store);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh(MESH);
        node.listen(host.equals("Relocating") ? "tcp://127.0.0.1:0" : "inproc://" + spotId)
                .setRoutingId(RoutingId.from(spotId));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addInstanceSpotFactory(
                TYPE, Instance.class, factory -> factory.disableRelocation());
        String name = branch == null ? "pending-before-close" : branch.path("name").asText();
        ZLinkFrameworkRuntime replacement = null;
        try (ArrivalLog flow = new ArrivalLog(spotId);
                ZLinkFrameworkRuntime runtime =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options, new ZLinkJavaBackendAdapterFactory())) {
            try {
                CompletableFuture<Reply> initial =
                        runtime.route()
                                .requestToSpot(spotId, new InitialProbe())
                                .instanceSpot(TYPE)
                                .inMesh(MESH)
                                .timeout(WAIT)
                                .submit(Reply.class)
                                .toCompletableFuture();
                observation.initialHandlerEntered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                ZLinkAuthoritySnapshot before = snapshot(repository, spotId);
                CompletableFuture<Reply> pending = null;
                if (queuedBeforeClose) {
                    pending = pending(runtime, spotId, true);
                    flow.ownerArrival.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    observation.activeRelease.complete(null);
                } else {
                    assertEquals(
                            before.objectGeneration(),
                            initial.get(WAIT.toSeconds(), TimeUnit.SECONDS).generation());
                    runtime.route()
                            .sendToSpot(spotId, new CloseProbe())
                            .instanceSpot(TYPE)
                            .inMesh(MESH)
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
                try {
                    CompletableFuture.anyOf(observation.closingEntered, observation.closeResult)
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                } catch (TimeoutException error) {
                    throw new AssertionError(
                            "Close progress timed out: events="
                                    + observation.events
                                    + ", closeResult.isDone="
                                    + observation.closeResult.isDone()
                                    + ", closingEntered.isDone="
                                    + observation.closingEntered.isDone(),
                            error);
                }
                assertTrue(
                        observation.closingEntered.isDone(),
                        "Close must enter OnClosing before its terminal result");
                observation.closingEntered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(0, observation.pendingGenerations.size());
                if (intent && !queuedBeforeClose) {
                    pending = pending(runtime, spotId, true);
                    flow.ownerArrival.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                } else if (name.equals("closing-message-without-intent")) {
                    pending = pending(runtime, spotId, false);
                    assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure(pending).kind());
                    assertEquals(1, observation.terminals.get());
                }
                CompletionStage<?> hostOperation = null;
                if (host.equals("Draining")) {
                    hostOperation = runtime.shutdown(WAIT);
                    assertEquals(ZLinkFrameworkRuntimeState.DRAINING, runtime.status().state());
                } else if (host.equals("Relocating")) {
                    DefaultZLinkFrameworkOptions replacementOptions =
                            new DefaultZLinkFrameworkOptions();
                    replacementOptions.addLocationStore(store.inner);
                    replacementOptions
                            .addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(RoutingId.from(spotId + "-replacement"))
                            .objects()
                            .server()
                            .addEntrySpot(Entry.class);
                    replacement =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    replacementOptions, new ZLinkJavaBackendAdapterFactory());
                    CompletableFuture<Void> relocating =
                            observeState(runtime, ZLinkFrameworkRuntimeState.RELOCATING);
                    hostOperation =
                            runtime.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            WAIT));
                    relocating.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
                observation.closingRelease.complete(null);
                if (failInitialization) {
                    observation
                            .closeResult
                            .handle(
                                    (result, error) -> {
                                        observation.events.add(
                                                "closeResult=" + result + ";error=" + error);
                                        flow.record("closeResult=" + result + ";error=" + error);
                                        return null;
                                    })
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                } else {
                    assertTrue(observation.closeResult.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                }
                boolean recreated = intent && host.equals("Serving") && !failInitialization;
                if (pending != null && !name.equals("closing-message-without-intent")) {
                    if (recreated) {
                        Reply reply = pending.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        assertNotEquals(before.objectGeneration(), reply.generation());
                        assertEquals("durable-state", reply.restored());
                        assertEquals(List.of(reply.generation()), observation.pendingGenerations);
                    } else {
                        ZLinkFrameworkException terminal = failure(pending);
                        if (failInitialization) {
                            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, terminal.kind());
                            assertInstanceOf(IllegalStateException.class, terminal.getCause());
                            assertTrue(terminal.metadata().isEmpty());
                        }
                    }
                    assertEquals(1, observation.terminals.get());
                }
                assertEquals(1, observation.closingCalls.get());
                assertTrue(observation.oldTimer.isDisposed());
                assertEquals(0, observation.timerCalls.get());
                assertOrdered(
                        observation.events,
                        List.of("closingCommitted", "onClosing", "localResourcesReleased"));
                assertFalse(observation.pendingGenerations.contains(before.objectGeneration()));
                if (recreated) {
                    ZLinkAuthoritySnapshot after = snapshot(repository, spotId);
                    assertNotEquals(before.objectGeneration(), after.objectGeneration());
                    assertNotEquals(
                            before.authorityOwnerGeneration(), after.authorityOwnerGeneration());
                    assertEquals(before.ownerId(), after.ownerId());
                    assertEquals(before.ownerLeaseGeneration(), after.ownerLeaseGeneration());
                    assertEquals(before.allocation(), after.allocation());
                    assertEquals(2, observation.initializations.size());
                    assertOrdered(
                            observation.events,
                            List.of(
                                    "authorityReincarnated",
                                    "newIncarnationInitialized",
                                    "storedStateRestored",
                                    "pendingIntentMessagesExecuted"));
                } else {
                    boolean missingPlacementBeforeInspection = flow.missingPlacement.isDone();
                    assertInstanceOf(
                            ZLinkAuthorityMissing.class,
                            repository
                                    .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS));
                    assertTrue(observation.pendingGenerations.isEmpty());
                    if (failInitialization) {
                        if (failConfigure) {
                            assertEquals(1, observation.initializations.size());
                            assertEquals(2, observation.configureGenerations.size());
                            assertNotEquals(
                                    before.objectGeneration(),
                                    observation.configureGenerations.getLast());
                            assertEquals(
                                    observation.configureGenerations.getLast(),
                                    observation.deletedGeneration);
                        } else {
                            assertEquals(2, observation.initializations.size());
                            assertNotEquals(
                                    before.objectGeneration(),
                                    observation.initializations.getLast());
                            assertEquals(
                                    observation.initializations.getLast(),
                                    observation.deletedGeneration);
                        }
                        assertOrdered(
                                observation.events,
                                List.of(
                                        "authorityReincarnated",
                                        "newGenerationDeleted",
                                        "pendingMessagesTypedFailure"));
                    } else {
                        assertEquals(1, observation.initializations.size());
                        assertTrue(observation.events.contains("authorityReleased"));
                    }
                    if (!host.equals("Serving")) {
                        assertTrue(
                                missingPlacementBeforeInspection,
                                "pending intent must enter existing Missing placement after release");
                    }
                }
                if (branch != null) {
                    JsonNode expected = branch.path("expect");
                    Set<String> expectedKeys =
                            Set.of(
                                    "order",
                                    "authority",
                                    "objectGeneration",
                                    "authorityOwnerGeneration",
                                    "owner",
                                    "lease",
                                    "capacity",
                                    "oldHandlerCalls",
                                    "newHandlerCalls",
                                    "factoryCalls",
                                    "thisHostFactoryCalls",
                                    "messageTerminal",
                                    "messageTerminalCount");
                    expected.fieldNames()
                            .forEachRemaining(
                                    key ->
                                            assertTrue(
                                                    expectedKeys.contains(key),
                                                    "unknown close branch expectation " + key));
                    List<String> order = new ArrayList<>();
                    expected.path("order").forEach(item -> order.add(item.asText()));
                    assertOrdered(observation.events, order);
                    if (expected.has("authority")) {
                        assertEquals(
                                recreated ? "Ready" : "Missing",
                                expected.path("authority").asText());
                    }
                    if (expected.has("factoryCalls")) {
                        assertEquals(
                                expected.path("factoryCalls").asInt(),
                                observation.initializations.size() - 1);
                    }
                    if (expected.has("thisHostFactoryCalls")) {
                        assertEquals(
                                expected.path("thisHostFactoryCalls").asInt(),
                                observation.initializations.size() - 1);
                    }
                    if (expected.has("objectGeneration")) {
                        assertEquals(
                                "storeIssuedDifferent", expected.path("objectGeneration").asText());
                        assertNotEquals(
                                before.objectGeneration(), observation.reincarnatedGeneration);
                        assertEquals(
                                observation.reincarnatedGeneration,
                                observation.initializations.getLast());
                    }
                    if (expected.has("authorityOwnerGeneration")) {
                        assertEquals(
                                "storeIssuedDifferent",
                                expected.path("authorityOwnerGeneration").asText());
                        assertNotEquals(
                                before.authorityOwnerGeneration(),
                                observation.reincarnatedOwnerGeneration);
                    }
                    if (expected.has("messageTerminal")) {
                        assertEquals(
                                name.equals("closing-message-without-intent")
                                        ? "NotFound"
                                        : recreated ? "reply" : "typedFailure",
                                expected.path("messageTerminal").asText());
                    }
                    assertEquals(
                            expected.path("oldHandlerCalls").asInt(),
                            observation.pendingGenerations.stream()
                                    .filter(g -> g == before.objectGeneration())
                                    .count());
                    if (expected.has("newHandlerCalls")) {
                        assertEquals(
                                expected.path("newHandlerCalls").asInt(),
                                observation.pendingGenerations.size());
                    }
                    if (expected.has("messageTerminalCount")) {
                        assertEquals(
                                expected.path("messageTerminalCount").asInt(),
                                observation.terminals.get());
                    }
                }
                if (hostOperation != null) {
                    hostOperation.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
            } finally {
                observation.activeRelease.complete(null);
                observation.closingRelease.complete(null);
            }
        } finally {
            observation.activeRelease.complete(null);
            observation.closingRelease.complete(null);
            if (replacement != null) replacement.close();
        }
    }

    private static CompletableFuture<Reply> pending(
            ZLinkFrameworkRuntime runtime, String spotId, boolean intent) {
        var request =
                runtime.route()
                        .requestToSpot(spotId, new PendingProbe())
                        .inMesh(MESH)
                        .timeout(WAIT);
        if (intent) request = request.instanceSpot(TYPE);
        Observation observation = current;
        return request.submit(Reply.class)
                .whenComplete(
                        (reply, error) -> {
                            observation.terminals.incrementAndGet();
                            if (error != null)
                                observation.events.add("pendingMessagesTypedFailure");
                        })
                .toCompletableFuture();
    }

    private static ZLinkFrameworkException failure(CompletableFuture<?> future) {
        ExecutionException failed =
                assertThrows(
                        ExecutionException.class,
                        () -> future.get(WAIT.toSeconds(), TimeUnit.SECONDS));
        Throwable error = failed.getCause();
        while (error instanceof CompletionException) error = error.getCause();
        return assertInstanceOf(ZLinkFrameworkException.class, error);
    }

    private static ZLinkAuthoritySnapshot snapshot(
            ZLinkLocationRepository repository, String spotId) throws Exception {
        return assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository
                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS));
    }

    private static void assertOrdered(List<String> events, List<String> expected) {
        int previous = -1;
        for (String event : expected) {
            int index = events.indexOf(event);
            assertTrue(index > previous, "missing or unordered " + event + ": " + events);
            previous = index;
        }
    }

    private static CompletableFuture<Void> observeState(
            ZLinkFrameworkRuntime runtime, ZLinkFrameworkRuntimeState expected) {
        CompletableFuture<Void> observed = new CompletableFuture<>();
        runtime.observe()
                .subscribe(
                        new Flow.Subscriber<>() {
                            private Flow.Subscription subscription;

                            public void onSubscribe(Flow.Subscription next) {
                                subscription = next;
                                next.request(Long.MAX_VALUE);
                            }

                            public void onNext(
                                    systems.zlink.framework.monitoring.ZLinkObservedStatus<
                                                    systems.zlink.framework.monitoring
                                                            .ZLinkFrameworkRuntimeStatus>
                                            item) {
                                if (item.status().state() == expected) {
                                    observed.complete(null);
                                    subscription.cancel();
                                }
                            }

                            public void onError(Throwable error) {
                                observed.completeExceptionally(error);
                            }

                            public void onComplete() {
                                if (!observed.isDone())
                                    observed.completeExceptionally(
                                            new IllegalStateException(
                                                    "expected host state was not published"));
                            }
                        });
        return observed;
    }

    public record InitialProbe(String value) {
        public InitialProbe() {
            this("initial");
        }
    }

    public record PendingProbe(String value) {
        public PendingProbe() {
            this("pending");
        }
    }

    public record CloseProbe(String value) {
        public CloseProbe() {
            this("close");
        }
    }

    public record Reply(long generation, String restored) {}

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;
        final Observation observation = current;
        private String restored;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            observation.configureGenerations.add(context.objectGeneration());
            if (observation.failConfigure && !observation.initializations.isEmpty()) {
                throw new IllegalStateException("reincarnation configure failure");
            }
            context.handlers().addPacket(InitialHandler.class);
            context.handlers().addPacket(PendingHandler.class);
            context.handlers().addPacket(CloseHandler.class);
        }

        public CompletionStage<Void> onInitialize() {
            observation.initializations.add(context.objectGeneration());
            if (observation.initializations.size() > 1) {
                if (observation.failInitialization)
                    return CompletableFuture.failedFuture(
                            new IllegalStateException("reincarnation initialization failure"));
                observation.events.add("newIncarnationInitialized");
                restored = observation.durableState;
                observation.events.add("storedStateRestored");
            }
            if (observation.initializations.size() == 1) {
                return context.addTimer(
                                UNEXECUTED_TIMER_NAME,
                                UNEXECUTED_TIMER_PERIOD,
                                TimerHandler.class,
                                null)
                        .thenAccept(timer -> observation.oldTimer = timer);
            }
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            observation.closingCalls.incrementAndGet();
            observation.events.add("onClosing");
            observation.closingEntered.complete(null);
            return observation.closingRelease;
        }
    }

    public static final class InitialHandler
            implements ZLinkSpotRequestHandler<Instance, InitialProbe, Reply>, AutoCloseable {
        private final Observation observation = current;

        public CompletionStage<Reply> handle(Instance spot, InitialProbe request) {
            spot.observation.durableState = "durable-state";
            spot.observation.initialHandlerEntered.complete(null);
            if (!spot.observation.queuedBeforeClose)
                return CompletableFuture.completedFuture(
                        new Reply(spot.context.objectGeneration(), null));
            return spot.observation.activeRelease.thenApply(
                    ignored -> {
                        ZLinkInstanceSpotCloseConformanceTest.close(spot);
                        return new Reply(spot.context.objectGeneration(), null);
                    });
        }

        public void close() {
            observation.events.add("localResourcesReleased");
        }
    }

    public static final class PendingHandler
            implements ZLinkSpotRequestHandler<Instance, PendingProbe, Reply> {
        public CompletionStage<Reply> handle(Instance spot, PendingProbe request) {
            spot.observation.pendingGenerations.add(spot.context.objectGeneration());
            spot.observation.events.add("pendingIntentMessagesExecuted");
            return CompletableFuture.completedFuture(
                    new Reply(spot.context.objectGeneration(), spot.restored));
        }
    }

    public static final class CloseHandler implements ZLinkSpotPacketHandler<Instance, CloseProbe> {
        public CompletionStage<Void> handle(Instance spot, CloseProbe request) {
            close(spot);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TimerHandler {
        public CompletionStage<Void> handle(Instance spot, ZLinkTimerTick tick) {
            spot.observation.timerCalls.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    private static void close(Instance spot) {
        spot.context
                .close()
                .whenComplete(
                        (closed, error) -> {
                            if (error == null) spot.observation.closeResult.complete(closed);
                            else spot.observation.closeResult.completeExceptionally(error);
                        });
    }

    public static final class Entry implements ZLinkEntrySpot<ZLinkActor> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class Observation {
        final boolean failInitialization;
        boolean failConfigure;
        final boolean queuedBeforeClose;
        final List<Long> initializations = new CopyOnWriteArrayList<>();
        final List<Long> configureGenerations = new CopyOnWriteArrayList<>();
        final List<Long> pendingGenerations = new CopyOnWriteArrayList<>();
        final List<String> events = new CopyOnWriteArrayList<>();
        final AtomicInteger closingCalls = new AtomicInteger();
        final AtomicInteger terminals = new AtomicInteger();
        final AtomicInteger timerCalls = new AtomicInteger();
        final CompletableFuture<Void> initialHandlerEntered = new CompletableFuture<>();
        final CompletableFuture<Void> activeRelease = new CompletableFuture<>();
        final CompletableFuture<Void> closingEntered = new CompletableFuture<>();
        final CompletableFuture<Void> closingRelease = new CompletableFuture<>();
        final CompletableFuture<Boolean> closeResult = new CompletableFuture<>();
        volatile String durableState;
        volatile ZLinkTimer oldTimer;
        volatile long deletedGeneration;
        volatile long reincarnatedGeneration;
        volatile long reincarnatedOwnerGeneration;

        Observation(boolean failing, boolean queued) {
            failInitialization = failing;
            queuedBeforeClose = queued;
        }
    }

    private static final class ObservedStore implements ZLinkLocationStore {
        final ZLinkInMemoryProviderLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        private final String spotId;
        private final Observation observation;
        private long originalGeneration;
        private long lastGeneration;

        ObservedStore(String spotId, Observation observation) {
            this.spotId = spotId;
            this.observation = observation;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            boolean put =
                    request.mutations().stream()
                            .anyMatch(
                                    item ->
                                            item instanceof ZLinkStorePut write
                                                    && authority(write.key()));
            boolean delete =
                    request.mutations().stream()
                            .anyMatch(
                                    item ->
                                            item instanceof ZLinkStoreDelete write
                                                    && authority(write.key()));
            return inner.write(request, cancellation)
                    .thenCompose(
                            result -> {
                                if (!(result instanceof ZLinkStoreWriteApplied))
                                    return CompletableFuture.completedFuture(result);
                                if (delete) {
                                    observation.deletedGeneration = lastGeneration;
                                    observation.events.add(
                                            lastGeneration == originalGeneration
                                                    ? "authorityReleased"
                                                    : "newGenerationDeleted");
                                }
                                if (!put) return CompletableFuture.completedFuture(result);
                                return new ZLinkProviderLocationRepository(inner)
                                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                        .thenApply(
                                                read -> {
                                                    if (read
                                                            instanceof
                                                            ZLinkAuthoritySnapshot snapshot) {
                                                        var payload =
                                                                new ZLinkServiceAuthorityPayloadCodec()
                                                                        .decode(snapshot.payload());
                                                        if (payload.isPresent()
                                                                && payload.orElseThrow().state()
                                                                        == ZLinkServiceAuthorityPayloadCodec
                                                                                .State.CLOSING) {
                                                            observation.events.add(
                                                                    "closingCommitted");
                                                        }
                                                        if (originalGeneration == 0)
                                                            originalGeneration =
                                                                    snapshot.objectGeneration();
                                                        if (snapshot.objectGeneration()
                                                                        != originalGeneration
                                                                && snapshot.objectGeneration()
                                                                        != lastGeneration) {
                                                            observation.events.add(
                                                                    "authorityReincarnated");
                                                            observation.reincarnatedGeneration =
                                                                    snapshot.objectGeneration();
                                                            observation
                                                                            .reincarnatedOwnerGeneration =
                                                                    snapshot
                                                                            .authorityOwnerGeneration();
                                                        }
                                                        lastGeneration =
                                                                snapshot.objectGeneration();
                                                    }
                                                    return result;
                                                });
                            });
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        private boolean authority(ZLinkStoreKey key) {
            return key.value().startsWith("authority\0") && key.value().endsWith("\0" + spotId);
        }
    }

    private static final class ArrivalLog extends Handler implements AutoCloseable {
        private final Logger logger = Logger.getLogger(FLOW_LOGGER);
        private final String spotId;
        final CompletableFuture<String> ownerArrival = new CompletableFuture<>();
        final CompletableFuture<Void> missingPlacement = new CompletableFuture<>();
        private final java.io.BufferedWriter writer;

        ArrivalLog(String spotId) throws Exception {
            this.spotId = spotId;
            Path path = Path.of("build", spotId + ".flow");
            Files.createDirectories(path.getParent());
            writer = Files.newBufferedWriter(path);
            logger.addHandler(this);
        }

        public void publish(LogRecord record) {
            String line = record.getMessage();
            if (line == null) return;
            record(line);
            Map<String, String> fields = new HashMap<>();
            for (String field : line.split(" ")) {
                int separator = field.indexOf('=');
                if (separator >= 0)
                    fields.put(field.substring(0, separator), field.substring(separator + 1));
            }
            if ("received".equals(fields.get("phase"))
                    && spotId.equals(fields.get("spot"))
                    && PendingProbe.class.getSimpleName().equals(fields.get("packet"))
                    && fields.containsKey("corr")) {
                ownerArrival.complete(fields.get("corr"));
            }
            if (spotId.equals(fields.get("spot"))
                    && PendingProbe.class.getSimpleName().equals(fields.get("packet"))
                    && "missing".equals(fields.get("activation_state"))
                    && fields.containsKey("corr")
                    && ownerArrival.isDone()
                    && Objects.equals(ownerArrival.getNow(null), fields.get("corr"))) {
                current.events.add("missingPlacement");
                missingPlacement.complete(null);
            }
        }

        void record(String line) {
            try {
                synchronized (writer) {
                    writer.write(line);
                    writer.newLine();
                    writer.flush();
                }
            } catch (java.io.IOException error) {
                throw new java.io.UncheckedIOException(error);
            }
        }

        public void flush() {}

        public void close() {
            logger.removeHandler(this);
            try {
                writer.close();
            } catch (java.io.IOException error) {
                throw new java.io.UncheckedIOException(error);
            }
        }
    }
}
