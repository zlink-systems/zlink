package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateParticipant;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime.RelocationBlockedException;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.function.Consumer;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.stream.Collectors;

/** Public inputs for the source observation and current membership contracts (#1740). */
final class ZLinkRelocationObservationPublicTest {
    private static final String MESH = "observation";
    private static final String RELOCATION_FLOW = "01900000-0000-7000-8000-000000001882";
    private static final String RECREATED_ACTOR_ID = "player-recreated-after-join";
    static final Duration WAIT = Duration.ofSeconds(15);
    private static CompletableFuture<ZLinkActorJoinCompletion> joined;
    private static CompletableFuture<Void> sourceLeft;
    private static CompletableFuture<ZLinkSessionContext> connected;
    private static CompletableFuture<String> relayed;
    private static volatile Consumer<String> captureObserver = ignored -> {};
    private static volatile java.util.function.Function<String, CompletionStage<Void>> captureGate =
            ignored -> CompletableFuture.completedFuture(null);

    @Test
    void boundSessionRelaySurvivesActualUserSpotRelocation() throws Exception {
        joined = new CompletableFuture<>();
        connected = new CompletableFuture<>();
        relayed = new CompletableFuture<>();
        var publicationLaneExecutor =
                new java.util.concurrent.atomic.AtomicReference<java.util.concurrent.Executor>(
                        Runnable::run);
        var publicationLane =
                new ZLinkStateLane(task -> publicationLaneExecutor.get().execute(task));
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var publicationExecutor = Executors.newVirtualThreadPerTaskExecutor();
                var source =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options("source-bound", locations, relocations, null))) {
            awaitReady(source);
            source.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    source.actorManager()
                                            .create("player-bound", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS))
                            .actor();
            source.actorClient()
                    .requestToActor(actor.actorId(), new Join("room"))
                    .timeout(WAIT)
                    .submit(String.class)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            var gatewayOptions = new DefaultZLinkFrameworkOptions();
            gatewayOptions.addLocationStore(locations);
            gatewayOptions.addRelocationStore(relocations);
            gatewayOptions.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
            gatewayOptions.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var gatewayMesh =
                    gatewayOptions
                            .addRouteMesh(MESH)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(RoutingId.from("gateway-bound"));
            gatewayMesh.objects().client();
            gatewayMesh.peerConnections().connect(endpoint);
            var streamOptions = gatewayOptions.addStreamNode("gateway");
            streamOptions.bind("tcp://127.0.0.1:0");
            streamOptions.enableActorDispatch();
            streamOptions.registerSession(BoundSession.class);
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("target-bound", locations, relocations, endpoint));
                    var gateway =
                            ZLinkFrameworkRuntimeTestAccess.startHost(
                                    gatewayOptions,
                                    systems.zlink.framework.runtime.binding
                                            .ZLinkJavaBackendAdapterFactoryTestAccess
                                            .withRawSpotStateLaneFactory(() -> publicationLane));
                    var targetReady =
                            observe(
                                    target.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0);
                    var gatewayReady =
                            observe(
                                    gateway.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0)) {
                awaitReady(target);
                awaitReady(gateway);
                targetReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                gatewayReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var address =
                        URI.create(
                                gateway.listenerStatus(ZLinkListenerKind.STREAM, "gateway")
                                        .endpoint());
                try (var client = new Socket(address.getHost(), address.getPort())) {
                    var session = connected.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    var sessionActors = (ZLinkSessionActorsRuntime) session.actors();
                    var binding =
                            sessionActors
                                    .bind(actor)
                                    .toCompletableFuture()
                                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    var publicationEntered = new CompletableFuture<Void>();
                    var publicationRelease = new CompletableFuture<Void>();
                    publicationLaneExecutor.set(
                            task ->
                                    publicationExecutor.execute(
                                            () -> {
                                                publicationEntered.complete(null);
                                                publicationRelease.join();
                                                task.run();
                                            }));
                    CompletableFuture<Void> submitted;
                    try {
                        var result =
                                source.relocate(
                                                new ZLinkFrameworkRelocationOptions(
                                                        ZLinkFrameworkRelocationMode
                                                                .PLANNED_MAINTENANCE,
                                                        null,
                                                        WAIT))
                                        .toCompletableFuture()
                                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        assertEquals(ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome());
                        publicationEntered.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                        sessionActors.enterRelayDispatch(
                                new ZLinkStreamHeader(
                                                Probe.class.getSimpleName(),
                                                Map.of(),
                                                Optional.empty())
                                        .withFlow(RELOCATION_FLOW, ZLinkFlowOrigin.INBOUND));
                        try {
                            submitted =
                                    binding.relay(ZLinkMessage.of(new Probe("after-relocation")))
                                            .toCompletableFuture();
                        } finally {
                            sessionActors.exitRelayDispatch();
                        }
                        assertFalse(
                                submitted.isDone(),
                                "relay remains sealed while native publication is held");
                        systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntimeTestAccess
                                .expireFirstRelocationSeal(sessionActors);
                        assertTrue(
                                sessionActors.bound().contains(binding),
                                "accepted command 44 keeps the physical Session binding live while"
                                        + " native publication waits on its owner");
                    } finally {
                        publicationRelease.complete(null);
                    }
                    submitted.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(
                            "after-relocation", relayed.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                }
            }
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void capturedActorRequestReturnsTerminalToOriginalRequesterAcrossDeferredCrossNodeJoin(
            boolean remoteRequester) throws Exception {
        var locations = new ReplyTerminalGate();
        var relocations = new InMemoryRelocationStore();
        var captured = new CompletableFuture<Void>();
        var releaseCapture = new CompletableFuture<Void>();
        var actorId = "relocating-request-actor";
        joined = new CompletableFuture<>();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("reply-source", locations, relocations, null))) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("reply-room", "room").submit());
            await(source.actorManager().create(actorId, "player").submit());
            assertEquals(
                    "scheduled",
                    await(
                            source.actorClient()
                                    .requestToActor(actorId, new Join("reply-room"))
                                    .timeout(WAIT)
                                    .submit(String.class)));
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(joined));
            joined = new CompletableFuture<>();
            var sourceEndpoint =
                    source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            options("reply-target", locations, relocations, sourceEndpoint))) {
                awaitReady(target);
                var targetEndpoint =
                        target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
                try (var destination =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options(
                                        "reply-join-destination",
                                        locations,
                                        relocations,
                                        targetEndpoint,
                                        objects ->
                                                objects.addSpotFactory(
                                                        "join-only",
                                                        Room.class,
                                                        factory -> factory.disableRelocation())))) {
                    awaitReady(destination);
                    await(
                            destination
                                    .spotManager()
                                    .getOrCreate("reply-join-only", "join-only")
                                    .submit());
                    try (var sourceReady =
                                    observe(
                                            source.routeMeshRuntime().observe(MESH, 8),
                                            state ->
                                                    state.isReady()
                                                            && state.readyPeerCount()
                                                                    >= (remoteRequester ? 2 : 1));
                            var destinationReady =
                                    observe(
                                            destination.routeMeshRuntime().observe(MESH, 8),
                                            state ->
                                                    state.isReady()
                                                            && state.readyPeerCount()
                                                                    >= (remoteRequester ? 2 : 1))) {
                        await(sourceReady.result);
                        await(destinationReady.result);
                        captureGate =
                                id -> {
                                    if (id.equals(actorId) && captured.complete(null)) {
                                        return releaseCapture;
                                    }
                                    return CompletableFuture.completedFuture(null);
                                };
                        var first = source.relocate(relocationOptions());
                        try {
                            await(captured);
                            var reply =
                                    (remoteRequester ? destination : source)
                                            .actorClient()
                                            .requestToActor(actorId, new Join("reply-join-only"))
                                            .timeout(WAIT)
                                            .submit(String.class);
                            await(reply.admitted());
                            releaseCapture.complete(null);
                            assertEquals(
                                    ZLinkFrameworkRelocationOutcome.RELOCATED,
                                    await(first).outcome());
                            assertEquals("scheduled", await(reply));
                            await(locations.terminalWrite);
                            assertInstanceOf(
                                    ZLinkActorJoinCompletion.Accepted.class, await(joined));
                            var authority =
                                    assertInstanceOf(
                                            systems.zlink.framework.runtime.internal.locations
                                                    .ZLinkAuthoritySnapshot.class,
                                            await(
                                                    new systems.zlink.framework.runtime.internal
                                                                    .locations
                                                                    .ZLinkProviderLocationRepository(
                                                                    locations)
                                                            .read(
                                                                    ZLinkAuthorityKeyCodec.actor(
                                                                            actorId),
                                                                    () -> false)));
                            assertEquals(
                                    RoutingId.from("reply-join-destination"),
                                    authority.allocation().descriptor().rid());
                        } finally {
                            releaseCapture.complete(null);
                            locations.releaseWrite.complete(null);
                            captureGate = ignored -> CompletableFuture.completedFuture(null);
                        }
                    }
                }
            }
        }
    }

    private static final class ReplyTerminalGate implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore delegate = new ZLinkInMemoryLocationStore();
        private final CompletableFuture<Void> terminalWrite = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseWrite = new CompletableFuture<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            for (var mutation : request.mutations()) {
                if (mutation instanceof ZLinkStorePut put
                        && put.key().value().startsWith("$zlink/relocation-reply/")
                        && new String(put.bytes(), StandardCharsets.UTF_8)
                                .contains("\"state\":\"terminalReceived\"")) {
                    terminalWrite.complete(null);
                    return releaseWrite.thenCompose(
                            ignored -> delegate.write(request, cancellation));
                }
            }
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    @Test
    void vacatingIdleSpotBeforeRelocationKeepsTheRemainingWorkloadRelocatable() throws Exception {
        joined = new CompletableFuture<>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceOptions = options("vacate-source", locations, relocations, null);
        sourceOptions.registration().meshNodes().getFirst().setSpotCapacity(3);
        try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
            awaitReady(source);
            for (String room : java.util.List.of("room-1", "room-2", "room-3")) {
                source.spotManager()
                        .getOrCreate(room, "room")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            }
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    source.actorManager()
                                            .create("vacate-player", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS))
                            .actor();
            source.actorClient()
                    .requestToActor(actor.actorId(), new Join("room-3"))
                    .timeout(WAIT)
                    .submit(String.class)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            var targetOptions = options("vacate-target", locations, relocations, endpoint);
            targetOptions.registration().meshNodes().getFirst().setSpotCapacity(5);
            try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions);
                    var ready =
                            observe(
                                    target.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                awaitReady(target);
                ready.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                for (String room : java.util.List.of("room-4", "room-5", "room-6")) {
                    target.spotManager()
                            .getOrCreate(room, "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                }
                var idle =
                        source.spotManager()
                                .find("room-1")
                                .toCompletableFuture()
                                .join()
                                .orElseThrow();
                assertTrue(source.spotManager().close(idle).toCompletableFuture().join());
                var result =
                        source.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT))
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result.toString());
            }
        }
    }

    public record Probe(String value) {}

    public record BoundSession(ZLinkSessionContext context) implements ZLinkSession {
        @Override
        public CompletionStage<Void> onConnected() {
            connected.complete(context);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            relayed.completeExceptionally(
                    new IllegalStateException("physical Session disconnected"));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorSendHandler<Room, Player, Probe> {
        @Override
        public CompletionStage<Void> handle(
                Room room, Player actor, ZLinkMessageContext context, Probe message) {
            relayed.complete(message.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void existingObserverReceivesSourceQuiescenceAfterRelocated() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceOptions = options("source", locations, relocations, null);
        try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions);
                var heldPermit =
                        sourceOptions
                                .registration()
                                .applicationJobQueue()
                                .acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS)) {
            awaitReady(source);
            source.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("target", locations, relocations, endpoint));
                    var readySource =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0);
                    var readyTarget =
                            observe(
                                    target.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0);
                    var unsafe =
                            observe(
                                    source.observe(),
                                    s ->
                                            s.state() == ZLinkFrameworkRuntimeState.RELOCATED
                                                    && !s.safeToShutdown());
                    var safe =
                            observe(
                                    source.observe(),
                                    s ->
                                            s.state() == ZLinkFrameworkRuntimeState.RELOCATED
                                                    && s.safeToShutdown())) {
                awaitReady(target);
                readySource.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                readyTarget.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                var result =
                        source.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT))
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertEquals(ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome());
                var before = unsafe.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertFalse(
                        source.status().safeToShutdown(),
                        "Message Follow still retains source obligations");
                ZLinkFrameworkRuntimeStatus after;
                try {
                    after = safe.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                } catch (TimeoutException failure) {
                    assertTrue(source.status().safeToShutdown(), "source quiescence is queryable");
                    throw new AssertionError(
                            "existing observer did not receive Relocated/safeToShutdown=true",
                            failure);
                }
                assertTrue(
                        after.sequence() > before.sequence(),
                        "quiescence publishes a new Sequence");
                assertTrue(after.capacity().applicationJobQueue().reservedSupplyPermits() > 0);
                assertTrue(after.capacity().applicationJobQueue().permitsInUse() > 0);
                // SafeToShutdown covers relocation obligations, not application supply permits.
                // Capacity is a point-in-time value, and this source still claims each liveness
                // record from its target under a permit, so the queue is not asserted empty here.
                // Release publication without peer records is covered by
                // ZLinkFrameworkRuntimeStatusPublicationTest.
                heldPermit.close();
                var released = source.status();
                assertEquals(ZLinkFrameworkRuntimeState.RELOCATED, released.state());
                assertTrue(released.safeToShutdown());
                assertTrue(released.sequence() >= after.sequence());
            }
        }
    }

    @Test
    void findSpotReturnsTheCreatedSpotIncarnationAfterJoin() throws Exception {
        joined = new CompletableFuture<>();
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(
                                "membership",
                                new ZLinkInMemoryLocationStore(),
                                new InMemoryRelocationStore(),
                                null))) {
            awaitReady(runtime);
            var first =
                    runtime.spotManager()
                            .getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                            .spot();
            assertTrue(
                    runtime.spotManager()
                            .close(first)
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS));
            var current =
                    runtime.spotManager()
                            .getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                            .spot();
            assertNotEquals(first.objectGeneration(), current.objectGeneration());
            var actor =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    runtime.actorManager()
                                            .create("player", "player")
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT.toSeconds(), TimeUnit.SECONDS))
                            .actor();
            try {
                assertEquals(
                        "scheduled",
                        runtime.actorClient()
                                .requestToActor("player", new Join("room"))
                                .timeout(WAIT)
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS));
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                assertEquals(
                        current,
                        runtime.actorManager()
                                .findSpot("player")
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS)
                                .orElseThrow());
            } finally {
                joined = new CompletableFuture<>();
                runtime.actorClient()
                        .requestToActor("player", new Leave())
                        .timeout(WAIT)
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joined.get(WAIT.toSeconds(), TimeUnit.SECONDS));
                runtime.actorManager()
                        .destroy(actor)
                        .toCompletableFuture()
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void actorCanBeRecreatedOnTheJoinSourceAfterTargetShutdown() throws Exception {
        joined = new CompletableFuture<>();
        sourceLeft = new CompletableFuture<>();
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceOptions = options("recreate-source", locations, relocations, null, false);
        sourceOptions.configureLocations().setMessageFollowDuration(Duration.ofSeconds(30));
        try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions);
                var sourceServing = observe(source.observe(), status -> status.isReady())) {
            completeStartup(source);
            await(sourceServing.result);
            var original =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    await(
                                            source.actorManager()
                                                    .create(RECREATED_ACTOR_ID, "player")
                                                    .submit()))
                            .actor();
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("recreate-target", locations, relocations, endpoint));
                    var targetServing = observe(target.observe(), status -> status.isReady());
                    var sourcePeer =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0);
                    var targetPeer =
                            observe(
                                    target.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                completeStartup(target);
                await(targetServing.result);
                await(sourcePeer.result);
                await(targetPeer.result);
                var room =
                        await(target.spotManager().getOrCreate("recreate-room", "room").submit())
                                .spot();
                assertNotEquals(original.nodeRid(), room.nodeRid());
                assertEquals(
                        "scheduled",
                        await(
                                source.actorClient()
                                        .requestToActor(original.actorId(), new Join(room.spotId()))
                                        .timeout(WAIT)
                                        .submit(String.class)));
                assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(joined));
                await(sourceLeft);
                var moved = await(source.actorManager().find(original.actorId())).orElseThrow();
                assertEquals(room.nodeRid(), moved.nodeRid());
                assertEquals(original.objectGeneration(), moved.objectGeneration());
                assertEquals(
                        ZLinkFrameworkTerminationOutcome.STOPPED,
                        await(target.shutdown()).outcome());
            }

            assertTrue(await(source.actorManager().find(original.actorId())).isEmpty());
            var recreated =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    await(
                                            source.actorManager()
                                                    .getOrCreate(original.actorId(), "player")
                                                    .submit()))
                            .actor();
            assertEquals(original.nodeRid(), recreated.nodeRid());
            assertTrue(recreated.objectGeneration() > original.objectGeneration());
            assertEquals(
                    Long.valueOf(recreated.objectGeneration()),
                    await(
                            source.actorClient()
                                    .requestToActor(recreated.actorId(), new ReadGeneration())
                                    .timeout(WAIT)
                                    .submit(Long.class)));
        }
    }

    @Test
    void shutdownPreservesAnAcceptedSourceBeforeTheRestoreDeadline() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var repository = new ZLinkProviderLocationRepository(locations);
        var relocations = new InMemoryRelocationStore();
        var sourceUnits = new RelocationUnits();
        var targetUnits = new RelocationUnits();
        var commitEntered = new CompletableFuture<Void>();
        var releaseCommit = new CompletableFuture<Void>();
        var lateCommit = new CompletableFuture<ZLinkAggregateCommitResult>();
        var preserved = new CompletableFuture<ZLinkAuthoritySnapshot>();
        var key = ZLinkAuthorityKeyCodec.spot("draining-room");
        targetUnits.beforeCommit =
                () -> {
                    commitEntered.complete(null);
                    return releaseCommit;
                };
        targetUnits.afterCommit = lateCommit::complete;
        sourceUnits.beforeAbort =
                () ->
                        repository
                                .read(key, () -> false)
                                .thenAccept(
                                        read ->
                                                preserved.complete(
                                                        assertInstanceOf(
                                                                ZLinkAuthoritySnapshot.class,
                                                                read)));

        try (var source =
                startObserved(
                        options("draining-source", locations, relocations, null), sourceUnits)) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("draining-room", "room").submit());
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            startObserved(
                                    options("draining-target", locations, relocations, endpoint),
                                    targetUnits);
                    var ready =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                try {
                    awaitReady(target);
                    await(ready.result);
                    var relocation =
                            source.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            Duration.ofMinutes(1)));
                    await(commitEntered);
                    var before =
                            assertInstanceOf(
                                    ZLinkAuthoritySnapshot.class,
                                    await(repository.read(key, () -> false)));
                    var shutdown = source.shutdown(Duration.ofSeconds(3));
                    var stopped = await(shutdown);
                    assertTrue(preserved.isDone(), "Preserve must settle by the stop deadline");
                    var after = await(preserved);
                    assertEquals(before.ownerId(), after.ownerId());
                    assertEquals(before.ownerLeaseGeneration(), after.ownerLeaseGeneration());
                    assertEquals(before.objectGeneration(), after.objectGeneration());
                    assertEquals(
                            before.authorityOwnerGeneration(), after.authorityOwnerGeneration());
                    assertNotEquals(before.storeVersion(), after.storeVersion());
                    assertEquals("aborted", after.relocation().phase());
                    assertEquals(
                            ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED,
                            await(relocation).reason());
                    assertTrue(
                            ((ZLinkUserSpotRetireRuntime)
                                            ZLinkFrameworkRuntimeTestAccess.spotRetire(source))
                                    .relocationSourceQuiescent(),
                            "no source relocation may remain unsettled");
                    assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, stopped.outcome());
                    releaseCommit.complete(null);
                    assertEquals(ZLinkAggregateCommitResult.STALE, await(lateCommit));
                } finally {
                    releaseCommit.complete(null);
                }
            }
        }
    }

    @Test
    void perActorShellCommitsBeforeMembersMoveAsSeparateUnits() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var units = new RelocationUnits();
        var perActor = ZLinkAuthorityKeyCodec.spot("per-actor-room");
        var spotWide = ZLinkAuthorityKeyCodec.spot("wide-room");
        var first = ZLinkAuthorityKeyCodec.actor("member-a");
        var second = ZLinkAuthorityKeyCodec.actor("member-b");
        var wideMember = ZLinkAuthorityKeyCodec.actor("wide-member");
        Consumer<ZLinkMeshObjectServerBuilder> factories =
                objects -> {
                    addWideRoom(objects);
                    objects.addSpotFactory(
                            "per-actor",
                            PerActorRoom.class,
                            f -> {
                                f.executionMode(ZLinkUserSpotExecutionMode.PER_ACTOR);
                                f.recreateOnRelocation();
                            });
                };
        captureObserver = units::capture;
        try (var source =
                startObserved(
                        options("shell-source", locations, relocations, null, factories), units)) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("per-actor-room", "per-actor").submit());
            await(source.spotManager().getOrCreate("wide-room", "room").submit());
            createMember(source, "member-a", "per-actor-room");
            createMember(source, "member-b", "per-actor-room");
            createMember(source, "wide-member", "wide-room");

            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            startObserved(
                                    options(
                                            "shell-target",
                                            locations,
                                            relocations,
                                            endpoint,
                                            factories),
                                    units);
                    var ready =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0)) {
                awaitReady(target);
                await(ready.result);
                var result = await(source.relocate(relocationOptions()));
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result.toString());
                var shell = Set.of(perActor);
                var firstUnit = Set.of(first);
                var secondUnit = Set.of(second);
                var aggregate = Set.of(spotWide, wideMember);
                assertEquals(
                        Set.of(shell, firstUnit, secondUnit, aggregate),
                        units.events.stream()
                                .filter(event -> event.phase().equals("prepare"))
                                .map(UnitEvent::participants)
                                .collect(Collectors.toSet()),
                        units.events.toString());
                int shellCommit = units.events.indexOf(new UnitEvent("commit", shell));
                assertTrue(shellCommit >= 0, units.events.toString());
                int aggregatePrepare = units.events.indexOf(new UnitEvent("prepare", aggregate));
                for (var member : List.of(firstUnit, secondUnit)) {
                    int capture = units.events.indexOf(new UnitEvent("capture", member));
                    assertTrue(shellCommit < capture, units.events.toString());
                    int memberCommit = units.events.indexOf(new UnitEvent("commit", member));
                    assertTrue(
                            memberCommit >= 0 && memberCommit < aggregatePrepare,
                            units.events.toString());
                }
                for (String actorId : List.of("member-a", "member-b")) {
                    var membership = await(target.actorManager().findSpot(actorId)).orElseThrow();
                    assertEquals("per-actor-room", membership.spotId());
                    assertEquals(RoutingId.from("shell-target"), membership.nodeRid());
                }
            }
        } finally {
            captureObserver = ignored -> {};
        }
    }

    @Test
    void disabledStandaloneActorBlocksMixedHostBeforeAnyUnitStarts() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var units = new RelocationUnits();
        Consumer<ZLinkMeshObjectServerBuilder> factories =
                objects -> {
                    addWideRoom(objects);
                    objects.addActorFactory(
                            "disabled",
                            Player.class,
                            PlayerFactory.class,
                            f -> f.disableRelocation());
                };
        captureObserver = units::capture;
        try (var source =
                startObserved(
                        options("disabled-source", locations, relocations, null, factories),
                        units)) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("wide-room", "room").submit());
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    await(source.actorManager().create("disabled-actor", "disabled").submit()));
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    await(source.actorManager().create("enabled-actor", "player").submit()));

            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            startObserved(
                                    options(
                                            "disabled-target",
                                            locations,
                                            relocations,
                                            endpoint,
                                            factories),
                                    units);
                    var ready =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    s -> s.isReady() && s.readyPeerCount() > 0)) {
                awaitReady(target);
                await(ready.result);
                var result = await(source.relocate(relocationOptions()));
                assertAll(
                        () ->
                                assertEquals(
                                        ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome()),
                        () ->
                                assertEquals(
                                        ZLinkFrameworkRelocationReason.RELOCATION_DISABLED,
                                        result.reason()),
                        () ->
                                assertEquals(
                                        ZLinkFrameworkRuntimeState.SERVING,
                                        source.status().state()),
                        () -> assertEquals(List.of(), units.events));
            }
        } finally {
            captureObserver = ignored -> {};
        }
    }

    @Test
    void activeRecreateInstanceSpotDoesNotBlockUserSpotRelocation() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        Consumer<ZLinkMeshObjectServerBuilder> factories =
                objects -> {
                    addWideRoom(objects);
                    objects.addInstanceSpotFactory(
                            "instance",
                            ActiveInstance.class,
                            factory -> factory.recreateOnRelocation());
                };
        var sourceOptions = options("instance-source", locations, relocations, null, factories);
        try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("wide-room", "room").submit());
            assertEquals(
                    "instance-source",
                    await(
                            source.route()
                                    .requestToSpot("active-instance", new ReadInstanceOwner())
                                    .instanceSpot("instance")
                                    .inMesh(MESH)
                                    .timeout(WAIT)
                                    .submit(String.class)));

            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options(
                                            "instance-target",
                                            locations,
                                            relocations,
                                            endpoint,
                                            factories));
                    var ready =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                awaitReady(target);
                await(ready.result);
                var result = await(source.relocate(relocationOptions()));
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result.toString());
                var room = await(target.spotManager().getOrCreate("wide-room", "room").submit());
                assertEquals(RoutingId.from("instance-target"), room.spot().nodeRid());
                var instance =
                        new ZLinkProviderLocationRepository(locations)
                                .read(ZLinkAuthorityKeyCodec.spot("active-instance"), () -> false);
                assertEquals(
                        RoutingId.from("instance-target"),
                        assertInstanceOf(ZLinkAuthoritySnapshot.class, await(instance))
                                .allocation()
                                .descriptor()
                                .rid());
            }
        }
    }

    @Test
    void perActorShellDoesNotRestoreSourceSpotTimerRegistration() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        Consumer<ZLinkMeshObjectServerBuilder> factories =
                objects ->
                        objects.addSpotFactory(
                                "timer-room",
                                TimerRoom.class,
                                factory -> {
                                    factory.executionMode(ZLinkUserSpotExecutionMode.PER_ACTOR);
                                    factory.recreateOnRelocation();
                                });
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("timer-source", locations, relocations, null, factories))) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("timer-room", "timer-room").submit());
            assertTrue(readSpotTimerRegistration(source));
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options(
                                            "timer-target",
                                            locations,
                                            relocations,
                                            endpoint,
                                            factories));
                    var ready =
                            observe(
                                    source.routeMeshRuntime().observe(MESH, 8),
                                    status -> status.isReady() && status.readyPeerCount() > 0)) {
                awaitReady(target);
                await(ready.result);
                var result = await(source.relocate(relocationOptions()));
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result.toString());
                assertTrue(
                        readSpotTimerRegistration(target),
                        "the target can independently register the same key with a different"
                                + " handler");
            }
        }
    }

    @Test
    void missingFactoryPolicyUsesLegacyHostPreflightReasons() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("legacy-inventory", locations, relocations, null))) {
            awaitReady(source);
            await(source.spotManager().getOrCreate("legacy-room", "room").submit());

            // The inventory contains a live Room, and these registrations omit its policy.
            var mixedRegistrations =
                    options("legacy-inventory", locations, relocations, null, false)
                            .registration()
                            .meshNodes();
            assertEquals(
                    ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                    missingFactoryReason(source, mixedRegistrations));
            assertEquals(
                    ZLinkFrameworkRelocationReason.RELOCATION_DISABLED,
                    missingFactoryReason(source, List.of()));
        }
    }

    private static ZLinkFrameworkRelocationReason missingFactoryReason(
            ZLinkFrameworkRuntime host, List<MeshNodeRegistration> registrations) {
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkUserSpotRetireRuntime.preflightWithoutRelocationRuntime(
                                                ZLinkFrameworkRuntimeTestAccess.spots(host),
                                                ZLinkFrameworkRuntimeTestAccess.actors(host),
                                                registrations,
                                                () ->
                                                        fail(
                                                                "the legacy object must block"
                                                                        + " before target lookup"))
                                        .toCompletableFuture()
                                        .join());
        return assertInstanceOf(RelocationBlockedException.class, failure.getCause()).reason();
    }

    private static boolean readSpotTimerRegistration(ZLinkFrameworkRuntime host) throws Exception {
        return await(
                host.route()
                        .requestToSpot("timer-room", new ReadTimerRegistration())
                        .inMesh(MESH)
                        .timeout(WAIT)
                        .submit(Boolean.class));
    }

    private static void addWideRoom(ZLinkMeshObjectServerBuilder objects) {
        objects.addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
    }

    private static void createMember(ZLinkFrameworkRuntime source, String actorId, String spotId)
            throws Exception {
        assertInstanceOf(
                ZLinkActorCreateResult.Created.class,
                await(source.actorManager().create(actorId, "player").submit()));
        joined = new CompletableFuture<>();
        await(
                source.actorClient()
                        .requestToActor(actorId, new Join(spotId))
                        .timeout(WAIT)
                        .submit(String.class));
        assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(joined));
    }

    private static ZLinkFrameworkRelocationOptions relocationOptions() {
        return new ZLinkFrameworkRelocationOptions(
                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE, null, WAIT);
    }

    private static ZLinkFrameworkRuntime startObserved(
            DefaultZLinkFrameworkOptions options, RelocationUnits units) {
        return ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .aggregateRelocationAuthorityStoreDecorator(units::observe));
    }

    private record UnitEvent(String phase, Set<String> participants) {}

    private static final class RelocationUnits {
        final List<UnitEvent> events = new CopyOnWriteArrayList<>();
        private final Map<UUID, Set<String>> inventory = new ConcurrentHashMap<>();
        Supplier<CompletionStage<Void>> beforeCommit =
                () -> CompletableFuture.completedFuture(null);
        Consumer<ZLinkAggregateCommitResult> afterCommit = ignored -> {};
        Runnable beforeAbort = () -> {};

        void capture(String actorId) {
            events.add(new UnitEvent("capture", Set.of(ZLinkAuthorityKeyCodec.actor(actorId))));
        }

        ZLinkLocationRepository observe(ZLinkLocationRepository delegate) {
            return (ZLinkLocationRepository)
                    Proxy.newProxyInstance(
                            ZLinkLocationRepository.class.getClassLoader(),
                            new Class<?>[] {ZLinkLocationRepository.class},
                            (proxy, method, arguments) -> {
                                if (method.getName().equals("prepareAggregate")) {
                                    var request = (ZLinkAggregatePrepareRequest) arguments[0];
                                    var participants =
                                            request.participants().stream()
                                                    .map(ZLinkAggregateParticipant::authorityKey)
                                                    .collect(Collectors.toUnmodifiableSet());
                                    inventory.put(request.aggregateId(), participants);
                                    events.add(new UnitEvent("prepare", participants));
                                }
                                if (method.getName().equals("abortAggregate")) beforeAbort.run();
                                try {
                                    if (method.getName().equals("commitAggregate")) {
                                        var fence = (ZLinkAggregateFence) arguments[0];
                                        @SuppressWarnings("unchecked")
                                        var replyRows =
                                                (List<ZLinkRelocationReplyRow>) arguments[1];
                                        var cancellation =
                                                (systems.zlink.framework.runtime.internal.locations
                                                                .ZLinkStoreCancellation)
                                                        arguments[2];
                                        return beforeCommit
                                                .get()
                                                .thenCompose(
                                                        ignored ->
                                                                delegate.commitAggregate(
                                                                        fence,
                                                                        replyRows,
                                                                        cancellation))
                                                .thenApply(
                                                        committed -> {
                                                            afterCommit.accept(committed);
                                                            if (committed
                                                                    == ZLinkAggregateCommitResult
                                                                            .COMMITTED) {
                                                                events.add(
                                                                        new UnitEvent(
                                                                                "commit",
                                                                                inventory.get(
                                                                                        fence
                                                                                                .aggregateId())));
                                                            }
                                                            return committed;
                                                        });
                                    }
                                    return method.invoke(delegate, arguments);
                                } catch (InvocationTargetException failure) {
                                    throw failure.getCause();
                                }
                            });
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        return options(rid, locations, relocations, peer, true);
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer,
            boolean hostsRooms) {
        return options(
                rid,
                locations,
                relocations,
                peer,
                objects -> {
                    if (hostsRooms) addWideRoom(objects);
                });
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer,
            Consumer<ZLinkMeshObjectServerBuilder> factories) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
        options.configureLocations().setMessageFollowDuration(Duration.ofSeconds(5));
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                f -> f.preserveStateWith(PlayerState.class));
        factories.accept(objects);
        return options;
    }

    private static void awaitReady(ZLinkFrameworkRuntime runtime) {
        completeStartup(runtime);
        assertTrue(runtime.isReady());
    }

    private static void completeStartup(ZLinkFrameworkRuntime runtime) {
        ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime).toCompletableFuture().join();
        if (!runtime.isReady())
            assertTrue(
                    ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                            .toCompletableFuture()
                            .join());
    }

    static <T> Observation<T> observe(
            Flow.Publisher<ZLinkObservedStatus<T>> publisher, Predicate<T> predicate) {
        var observation = new Observation<T>(predicate);
        publisher.subscribe(observation);
        return observation;
    }

    static final class Observation<T>
            implements Flow.Subscriber<ZLinkObservedStatus<T>>, AutoCloseable {
        final CompletableFuture<T> result = new CompletableFuture<>();
        private final Predicate<T> predicate;
        private Flow.Subscription subscription;

        Observation(Predicate<T> predicate) {
            this.predicate = predicate;
        }

        @Override
        public void onSubscribe(Flow.Subscription subscription) {
            this.subscription = subscription;
            subscription.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<T> observed) {
            if (predicate.test(observed.status())) result.complete(observed.status());
        }

        @Override
        public void onError(Throwable failure) {
            result.completeExceptionally(failure);
        }

        @Override
        public void onComplete() {}

        @Override
        public void close() {
            subscription.cancel();
        }
    }

    public record Join(String spotId) {}

    public record Leave() {}

    public record ReadGeneration() {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joined.complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class PlayerState implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            captureObserver.accept(actor.context().actorId());
            return captureGate.apply(actor.context().actorId()).thenApply(ignored -> new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Lobby(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            if (RECREATED_ACTOR_ID.equals(actor.context().actorId())) sourceLeft.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
            context.handlers().addHandler(ReadGenerationHandler.class);
        }
    }

    public record ReadInstanceOwner() {}

    public record ActiveInstance(ZLinkInstanceSpotContext context) implements ZLinkInstanceSpot {
        @Override
        public void configure() {
            context.handlers().addPacket(ReadInstanceOwnerHandler.class);
        }
    }

    public static final class ReadInstanceOwnerHandler
            implements ZLinkSpotRequestHandler<ActiveInstance, ReadInstanceOwner, String> {
        @Override
        public CompletionStage<String> handle(ActiveInstance spot, ReadInstanceOwner request) {
            return CompletableFuture.completedFuture(spot.context().nodeRid().toString());
        }
    }

    public record ReadTimerRegistration() {}

    public static final class TimerRoom implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;
        private boolean registered;

        public TimerRoom(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ReadTimerRegistrationHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            Class<?> handler =
                    context.nodeRid().equals(RoutingId.from("timer-source"))
                            ? SourceSpotTimer.class
                            : TargetSpotTimer.class;
            return context.addTimer("spot-timer", Duration.ofDays(1), handler, null)
                    .handle(
                            (timer, failure) -> {
                                registered = failure == null;
                                return null;
                            });
        }
    }

    public static final class ReadTimerRegistrationHandler
            implements ZLinkSpotRequestHandler<TimerRoom, ReadTimerRegistration, Boolean> {
        @Override
        public CompletionStage<Boolean> handle(TimerRoom spot, ReadTimerRegistration request) {
            return CompletableFuture.completedFuture(spot.registered);
        }
    }

    public static class SourceSpotTimer implements ZLinkSpotTimerHandler<TimerRoom> {
        @Override
        public CompletionStage<Void> handle(TimerRoom spot, ZLinkTimerTick tick) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TargetSpotTimer extends SourceSpotTimer {}

    public record PerActorRoom(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(LeaveHandler.class);
            context.handlers().addHandler(RelocatedJoinHandler.class);
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class RelocatedJoinHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(request.spotId()).timeout(WAIT).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class LeaveHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Leave, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Leave request) {
            actor.context().joinEntrySpot().defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ReadGenerationHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, ReadGeneration, Long> {
        @Override
        public CompletionStage<Long> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, ReadGeneration request) {
            return CompletableFuture.completedFuture(actor.context().objectGeneration());
        }
    }
}
