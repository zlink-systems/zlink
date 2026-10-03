package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.binding.ZLinkJavaReadyRouteTestAccess;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
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
    private static final String RELOCATION_HOLD_TYPE = "close-relocation-hold";
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

    @Test
    void pendingIntentRequestDuringHostDrainEndsWithShuttingDown() throws Exception {
        run(null, "Draining", true, false, false);
    }

    @Test
    void pendingIntentRequestDuringRelocationEndsWithUnavailable() throws Exception {
        run(null, "Relocating", true, false, false);
    }

    static void runReadyRouteCase(JsonNode routeCase) throws Exception {
        JsonNode given = routeCase.path("given");
        JsonNode expected = routeCase.path("expect");
        assertEquals("mismatch", given.path("ownerFence").asText());
        for (JsonNode intent : given.path("instanceIntent")) {
            String spotId = "java-stale-ready-" + UUID.randomUUID();
            Observation observation = new Observation(false, false);
            current = observation;
            ObservedStore store = new ObservedStore(spotId, observation);
            var repository = new ZLinkProviderLocationRepository(store);
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store);
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            options.addRouteMesh(MESH)
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(RoutingId.from(spotId))
                    .objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addInstanceSpotFactory(
                            TYPE, Instance.class, factory -> factory.disableRelocation());
            CapturingBackend backend = new CapturingBackend();
            try (ArrivalLog flow = new ArrivalLog(spotId);
                    ZLinkFrameworkRuntime runtime =
                            ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
                try {
                    runtime.route()
                            .requestToSpot(spotId, new InitialProbe())
                            .instanceSpot(TYPE)
                            .inMesh(MESH)
                            .timeout(WAIT)
                            .submit(Reply.class)
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    var before = snapshot(repository, spotId);
                    var authority =
                            new ZLinkServiceAuthorityPayloadCodec()
                                    .decode(before.payload())
                                    .orElseThrow();
                    var route =
                            new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                                    authority.nodeRid(),
                                    authority.nodeGeneration(),
                                    spotId,
                                    before.objectGeneration(),
                                    before.ownerId(),
                                    before.authorityOwnerGeneration() + 1,
                                    before.ownerLeaseGeneration(),
                                    before.storeVersion());
                    ZLinkStoreReadResult readyRecord = store.currentAuthority();
                    if (given.path("authority").asText().equals("Missing")) {
                        runtime.route()
                                .sendToSpot(spotId, new CloseProbe())
                                .instanceSpot(TYPE)
                                .inMesh(MESH)
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        observation.closingEntered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        observation.closingRelease.complete(null);
                        assertTrue(observation.closeResult.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                        assertInstanceOf(
                                ZLinkAuthorityMissing.class,
                                repository
                                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                        .toCompletableFuture()
                                        .get(WAIT.toSeconds(), TimeUnit.SECONDS));
                    }
                    int factoriesBefore = observation.configureGenerations.size();
                    int handlersBefore = observation.pendingGenerations.size();
                    int placementBefore = store.missingPlacementAttempts.get();
                    AtomicInteger terminalCount = new AtomicInteger();
                    ZLinkFrameworkException failure = null;
                    String sendDiagnostic = null;
                    boolean replied = false;
                    if (given.path("messageKind").asText().equals("request")
                            && intent.asBoolean()
                            && given.path("authority").asText().equals("Missing")) {
                        //  The caller read the released Ready record and sends to its fence.
                        store.staleAuthorityRead.set(readyRecord);
                        try {
                            runtime.route()
                                    .requestToSpot(spotId, new PendingProbe())
                                    .instanceSpot(TYPE)
                                    .inMesh(MESH)
                                    .timeout(WAIT)
                                    .submit(Reply.class)
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            replied = true;
                        } catch (java.util.concurrent.ExecutionException terminal) {
                            failure =
                                    assertInstanceOf(
                                            ZLinkFrameworkException.class, terminal.getCause());
                        }
                        terminalCount.incrementAndGet();
                    } else if (given.path("messageKind").asText().equals("request")) {
                        Throwable terminal =
                                ZLinkJavaReadyRouteTestAccess.rejectReadyRequest(
                                                backend.mesh,
                                                route,
                                                intent.asBoolean(),
                                                terminalCount)
                                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        failure = assertInstanceOf(ZLinkFrameworkException.class, terminal);
                    } else {
                        var sourceOptions = new DefaultZLinkFrameworkOptions();
                        sourceOptions.addLocationStore(store.inner);
                        sourceOptions
                                .addRouteMesh(MESH)
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from(spotId + "-source"))
                                .objects()
                                .server()
                                .addEntrySpot(Entry.class);
                        CapturingBackend sourceBackend = new CapturingBackend();
                        try (var sourceRuntime =
                                ZLinkFrameworkRuntimeTestAccess.start(
                                        sourceOptions, sourceBackend)) {
                            observeUntil(
                                            runtime.routeMeshRuntime().observe(MESH, 8),
                                            status ->
                                                    status.isReady() && status.readyPeerCount() > 0)
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            observeUntil(
                                            sourceRuntime.routeMeshRuntime().observe(MESH, 8),
                                            status ->
                                                    status.isReady() && status.readyPeerCount() > 0)
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            long sourceGeneration =
                                    backend.mesh.peers().stream()
                                            .filter(
                                                    peer ->
                                                            peer.routingId()
                                                                    .equals(
                                                                            sourceBackend.mesh
                                                                                    .routingId()))
                                            .findFirst()
                                            .orElseThrow()
                                            .lifecycleGeneration();
                            var serializer = new ZLinkJsonMessageSerializer();
                            try (Message payload =
                                    Message.from(
                                            serializer.serialize(new PendingProbe()).bytes())) {
                                List<Message> parts =
                                        new ZLinkSpotRouteMessages(serializer)
                                                .encodeSend(
                                                        MESH,
                                                        Optional.of(
                                                                PendingProbe.class.getSimpleName()),
                                                        payload,
                                                        null,
                                                        Map.of(),
                                                        null);
                                try {
                                    ZLinkJavaReadyRouteTestAccess.sendReady(
                                                    sourceBackend.mesh,
                                                    route,
                                                    sourceGeneration,
                                                    intent.asBoolean(),
                                                    parts)
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                                } finally {
                                    parts.forEach(Message::close);
                                }
                            }
                            String dropped =
                                    flow.sendDropped.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            assertTrue(
                                    dropped.contains("error_type=ZLinkFrameworkException"),
                                    dropped);
                            sendDiagnostic = dropped;
                        }
                    }
                    if (given.path("messageKind").asText().equals("send"))
                        assertEquals(1, flow.sendDiagnosticCount.get());
                    for (var fields = expected.fields(); fields.hasNext(); ) {
                        var field = fields.next();
                        switch (field.getKey()) {
                            case "messageTerminal" ->
                                    assertEquals(
                                            field.getValue().asText(),
                                            replied
                                                    ? "reply"
                                                    : failure.kind()
                                                                    == ZLinkFrameworkErrorKind
                                                                            .UNAVAILABLE
                                                            ? "Unavailable"
                                                            : failure.kind().toString());
                            case "messageTerminalCount" ->
                                    assertEquals(field.getValue().asInt(), terminalCount.get());
                            case "surface", "reason" ->
                                    assertTrue(
                                            sendDiagnostic.contains(
                                                    field.getKey()
                                                            + "="
                                                            + field.getValue().asText()),
                                            sendDiagnostic);
                            case "diagnostics" ->
                                    assertTrue(
                                            sendDiagnostic.contains(
                                                    "error_type=ZLinkFrameworkException"),
                                            sendDiagnostic);
                            case "handlerCalls" ->
                                    assertEquals(
                                            field.getValue().asInt(),
                                            observation.pendingGenerations.size() - handlersBefore);
                            case "factoryCalls" ->
                                    assertEquals(
                                            field.getValue().asInt(),
                                            observation.configureGenerations.size()
                                                    - factoriesBefore);
                            case "missingPlacementCalls" ->
                                    assertEquals(
                                            field.getValue().asInt(),
                                            store.missingPlacementAttempts.get() - placementBefore);
                            default -> fail("unknown Ready route expectation " + field.getKey());
                        }
                    }
                } finally {
                    observation.activeRelease.complete(null);
                    observation.closingRelease.complete(null);
                }
            }
        }
    }

    private static final class CapturingBackend implements ZLinkBackendAdapterProvider {
        private final ZLinkJavaBackendAdapterFactory delegate =
                new ZLinkJavaBackendAdapterFactory();
        ZLinkInternalMeshNode mesh;

        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createChannelAdapter(options);
        }

        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createSpotAdapter(options);
        }

        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createStreamAdapter(options);
        }

        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return delegate.createMonitoringAdapter(options);
        }

        public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
            var adapter = delegate.createMeshAdapter(options);
            return (context, name) -> {
                mesh = adapter.createMeshNode(context, name);
                return mesh;
            };
        }

        public java.util.function.Function<ZLinkBackendObject, Duration> admissionTimeout() {
            return delegate.admissionTimeout();
        }
    }

    static void runBranch(JsonNode branch) throws Exception {
        String name = branch.path("name").asText();
        assertTrue(BRANCHES.contains(name), "unknown close branch " + name);
        if (name.equals("release-during-host-drain-or-relocation")) {
            for (JsonNode host : branch.path("given").path("host")) {
                if (host.asText().equals("Relocating")) {
                    for (JsonNode seal : branch.path("given").path("relocationSeal"))
                        run(branch, host.asText(), true, false, false, false, seal.asText());
                } else {
                    run(branch, host.asText(), true, false, false);
                }
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
        run(branch, host, intent, queuedBeforeClose, failInitialization, failConfigure, "before");
    }

    private static void run(
            JsonNode branch,
            String host,
            boolean intent,
            boolean queuedBeforeClose,
            boolean failInitialization,
            boolean failConfigure,
            String relocationSeal)
            throws Exception {
        String spotId = "java-close-instance-" + UUID.randomUUID();
        Observation observation = new Observation(failInitialization, queuedBeforeClose);
        observation.failConfigure = failConfigure;
        current = observation;
        ObservedStore store = new ObservedStore(spotId, observation);
        ZLinkProviderLocationRepository repository = new ZLinkProviderLocationRepository(store);
        var relocationStore = new systems.zlink.framework.runtime.InMemoryRelocationStore();
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
        DefaultZLinkFrameworkOptions targetOptions = null;
        if (host.equals("Relocating")) {
            options.addRelocationStore(relocationStore);
            addRelocationHold(objects);
            targetOptions = new DefaultZLinkFrameworkOptions();
            targetOptions.addLocationStore(store.inner);
            targetOptions.addRelocationStore(relocationStore);
            var targetObjects =
                    targetOptions
                            .addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(RoutingId.from(spotId + "-target"))
                            .objects()
                            .server();
            targetObjects.addEntrySpot(Entry.class);
            addRelocationHold(targetObjects);
        }
        String name = branch == null ? "pending-before-close" : branch.path("name").asText();
        try (ArrivalLog flow = new ArrivalLog(spotId);
                ZLinkFrameworkRuntime runtime =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options, new ZLinkJavaBackendAdapterFactory())) {
            try {
                if (host.equals("Relocating")) {
                    String holdId = spotId + "-hold";
                    var hold =
                            runtime.spotManager()
                                    .getOrCreate(holdId, RELOCATION_HOLD_TYPE)
                                    .submit()
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(
                            RoutingId.from(spotId),
                            hold.spot().nodeRid(),
                            "Relocating fixture hold must belong to the source host");
                    runtime.route()
                            .requestToSpot(holdId, new InitialProbe())
                            .inMesh(MESH)
                            .timeout(WAIT)
                            .submit(Reply.class)
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
                try (ZLinkFrameworkRuntime target =
                        targetOptions == null
                                ? null
                                : ZLinkFrameworkRuntimeTestAccess.start(
                                        targetOptions, new ZLinkJavaBackendAdapterFactory())) {
                    if (host.equals("Relocating")) {
                        observeUntil(
                                        runtime.routeMeshRuntime().observe(MESH, 8),
                                        status -> status.isReady() && status.readyPeerCount() > 0)
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        observeUntil(
                                        target.routeMeshRuntime().observe(MESH, 8),
                                        status -> status.isReady() && status.readyPeerCount() > 0)
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    }
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
                        if (!host.equals("Serving")) {
                            runtime.route()
                                    .sendToSpot(spotId, new PendingProbe())
                                    .instanceSpot(TYPE)
                                    .inMesh(MESH)
                                    .submit()
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            flow.sendArrival.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        }
                    } else if (name.equals("closing-message-without-intent")) {
                        pending = pending(runtime, spotId, false);
                        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure(pending).kind());
                        assertEquals(1, observation.terminals.get());
                    }
                    CompletionStage<?> hostOperation = null;
                    if (host.equals("Draining")) {
                        hostOperation = runtime.shutdown(WAIT);
                        awaitHostState(runtime, ZLinkFrameworkRuntimeState.DRAINING, hostOperation);
                    } else if (host.equals("Relocating")) {
                        hostOperation =
                                runtime.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT));
                        awaitHostState(
                                runtime, ZLinkFrameworkRuntimeState.RELOCATING, hostOperation);
                    }
                    if (host.equals("Relocating")) {
                        // Only the before/after seal fixture needs private mesh drain access.
                        var field = ZLinkFrameworkRuntime.class.getDeclaredField("meshDrains");
                        field.setAccessible(true);
                        var drains =
                                (systems.zlink.framework.runtime.internal.drain
                                                .ZLinkMeshDrainCoordinator)
                                        field.get(runtime);
                        assertFalse(drains.isSealed(MESH));
                        if (relocationSeal.equals("after")) {
                            drains.seal(MESH);
                            assertTrue(drains.isSealed(MESH));
                        }
                    }
                    observation.closingRelease.complete(null);
                    if (failInitialization) {
                        observation
                                .closeResult
                                .handle(
                                        (result, error) -> {
                                            observation.events.add(
                                                    "closeResult=" + result + ";error=" + error);
                                            flow.record(
                                                    "closeResult=" + result + ";error=" + error);
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
                            assertEquals(
                                    List.of(reply.generation()), observation.pendingGenerations);
                        } else {
                            ZLinkFrameworkException terminal = failure(pending);
                            if (failInitialization) {
                                assertEquals(
                                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE, terminal.kind());
                                assertInstanceOf(IllegalStateException.class, terminal.getCause());
                                assertTrue(terminal.metadata().isEmpty());
                            } else if (!host.equals("Serving")) {
                                assertEquals(
                                        host.equals("Draining")
                                                ? ZLinkFrameworkErrorKind.SHUTTING_DOWN
                                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        terminal.kind());
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
                                before.authorityOwnerGeneration(),
                                after.authorityOwnerGeneration());
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
                        if (!host.equals("Serving"))
                            assertFalse(
                                    missingPlacementBeforeInspection,
                                    "accepted intent must not enter Missing placement after release");
                        if (!host.equals("Serving")) {
                            String dropped =
                                    flow.sendDropped.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            assertTrue(dropped.contains("action=drop"));
                            assertTrue(
                                    dropped.contains(
                                            "reason="
                                                    + (host.equals("Draining")
                                                            ? "shutdown"
                                                            : "stale_target")));
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
                                        "messageTerminalCount",
                                        "messageTerminalByHost",
                                        "sendDiagnosticsByHost",
                                        "missingPlacementCalls");
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
                                    "storeIssuedDifferent",
                                    expected.path("objectGeneration").asText());
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
                        if (expected.has("messageTerminalByHost")) {
                            String terminal =
                                    host.equals("Draining") ? "ShuttingDown" : "Unavailable";
                            assertEquals(
                                    expected.path("messageTerminalByHost").path(host).asText(),
                                    terminal);
                            assertEquals(
                                    terminal.equals("ShuttingDown")
                                            ? ZLinkFrameworkErrorKind.SHUTTING_DOWN
                                            : ZLinkFrameworkErrorKind.UNAVAILABLE,
                                    failure(pending).kind());
                        }
                        if (expected.has("sendDiagnosticsByHost")) {
                            String dropped =
                                    flow.sendDropped.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                            JsonNode diagnostic = expected.path("sendDiagnosticsByHost").path(host);
                            assertTrue(
                                    dropped.contains("error_type=ZLinkFrameworkException"),
                                    dropped);
                            assertTrue(
                                    dropped.contains(
                                            "surface=" + diagnostic.path("surface").asText()),
                                    dropped);
                            assertTrue(
                                    dropped.contains(
                                            "reason=" + diagnostic.path("reason").asText()),
                                    dropped);
                        }
                        if (expected.has("missingPlacementCalls")) {
                            assertEquals(
                                    expected.path("missingPlacementCalls").asInt(),
                                    flow.missingPlacementCalls.get());
                        }
                        if (expected.has("messageTerminalCount")) {
                            assertEquals(
                                    expected.path("messageTerminalCount").asInt(),
                                    observation.terminals.get());
                        }
                    }
                    if (host.equals("Relocating")) {
                        runtime.shutdown(WAIT)
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    }
                    if (hostOperation != null) {
                        hostOperation.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    }
                    if (!host.equals("Serving")) assertEquals(1, flow.sendDiagnosticCount.get());
                }
            } finally {
                observation.activeRelease.complete(null);
                observation.closingRelease.complete(null);
            }
        } finally {
            observation.activeRelease.complete(null);
            observation.closingRelease.complete(null);
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
                            if (error != null) {
                                observation.events.add(
                                        observation.events.contains("authorityReleased")
                                                ? "pendingMessagesTerminated"
                                                : "pendingMessagesTypedFailure");
                            }
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

    private static void awaitHostState(
            ZLinkFrameworkRuntime runtime,
            ZLinkFrameworkRuntimeState expected,
            CompletionStage<?> operation)
            throws Exception {
        CompletableFuture<Void> reached =
                observeUntil(runtime.observe(), status -> status.state() == expected);
        try {
            CompletableFuture.anyOf(reached, operation.toCompletableFuture())
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertTrue(
                    reached.isDone(),
                    "Host operation completed before "
                            + expected
                            + ": "
                            + operation.toCompletableFuture().getNow(null));
            reached.get();
        } finally {
            reached.cancel(false);
        }
    }

    private static <T> CompletableFuture<Void> observeUntil(
            Flow.Publisher<systems.zlink.framework.monitoring.ZLinkObservedStatus<T>> publisher,
            java.util.function.Predicate<T> accepted) {
        CompletableFuture<Void> reached = new CompletableFuture<>();
        reached.orTimeout(WAIT.toSeconds(), TimeUnit.SECONDS);
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription subscription) {
                        reached.whenComplete((ignored, error) -> subscription.cancel());
                        subscription.request(Long.MAX_VALUE);
                    }

                    public void onNext(
                            systems.zlink.framework.monitoring.ZLinkObservedStatus<T> value) {
                        if (accepted.test(value.status())) reached.complete(null);
                    }

                    public void onError(Throwable error) {
                        reached.completeExceptionally(error);
                    }

                    public void onComplete() {
                        if (!reached.isDone())
                            reached.completeExceptionally(
                                    new AssertionError(
                                            "Status observation completed before its condition"));
                    }
                });
        return reached;
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

    private static void addRelocationHold(
            systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder objects) {
        objects.addSpotFactory(
                RELOCATION_HOLD_TYPE,
                RelocationHold.class,
                factory -> {
                    factory.executionMode(
                            systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
                                    .SPOT_WIDE);
                    factory.relocationCoordinationMode(
                            systems.zlink.framework.configuration
                                    .ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED);
                    factory.recreateOnRelocation();
                });
    }

    public static final class RelocationHold implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public RelocationHold(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(RelocationHoldHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RelocationHoldHandler
            implements ZLinkSpotRequestHandler<RelocationHold, InitialProbe, Reply> {
        public CompletionStage<Reply> handle(RelocationHold spot, InitialProbe request) {
            return CompletableFuture.completedFuture(new Reply(0, null));
        }
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
        final AtomicInteger missingPlacementAttempts = new AtomicInteger();
        /** One authority read answered with an earlier record, as a caller's racing read. */
        final java.util.concurrent.atomic.AtomicReference<ZLinkStoreReadResult>
                staleAuthorityRead = new java.util.concurrent.atomic.AtomicReference<>();
        private volatile ZLinkStoreKey authorityKey;

        ObservedStore(String spotId, Observation observation) {
            this.spotId = spotId;
            this.observation = observation;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            if (authority(key)) {
                authorityKey = key;
                ZLinkStoreReadResult stale = staleAuthorityRead.getAndSet(null);
                if (stale != null) return CompletableFuture.completedFuture(stale);
            }
            return inner.read(key, cancellation);
        }

        ZLinkStoreReadResult currentAuthority() throws Exception {
            return inner.read(authorityKey, () -> false)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            if (request.conditions().stream()
                    .anyMatch(
                            condition ->
                                    condition instanceof ZLinkStoreMissingCondition missing
                                            && authority(missing.key())))
                missingPlacementAttempts.incrementAndGet();
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
        final CompletableFuture<Void> sendArrival = new CompletableFuture<>();
        final CompletableFuture<String> sendDropped = new CompletableFuture<>();
        final AtomicInteger sendDiagnosticCount = new AtomicInteger();
        final CompletableFuture<Void> missingPlacement = new CompletableFuture<>();
        final AtomicInteger missingPlacementCalls = new AtomicInteger();
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
            if ("received".equals(fields.get("phase"))
                    && spotId.equals(fields.get("spot"))
                    && "send".equals(fields.get("kind"))
                    && PendingProbe.class.getSimpleName().equals(fields.get("packet"))) {
                sendArrival.complete(null);
            }
            if (line.contains("event_id=zlink.dispatch_error")
                    && spotId.equals(fields.get("spot"))
                    && "send".equals(fields.get("kind"))) {
                sendDiagnosticCount.incrementAndGet();
                sendDropped.complete(line);
            }
            if (spotId.equals(fields.get("spot"))
                    && PendingProbe.class.getSimpleName().equals(fields.get("packet"))
                    && "missing".equals(fields.get("activation_state"))
                    && fields.containsKey("corr")
                    && ownerArrival.isDone()
                    && Objects.equals(ownerArrival.getNow(null), fields.get("corr"))) {
                missingPlacementCalls.incrementAndGet();
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
