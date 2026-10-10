package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkEndedInstanceIntentTest {
    private static final AtomicInteger factories = new AtomicInteger();

    @ParameterizedTest
    @CsvSource({"provider, false", "provider, true", "memory, false", "memory, true"})
    void endedSteadyReadyInstanceIntentCreatesNewGenerationAndRunsOneFactory(
            String backend, boolean fromClientNode) throws Exception {
        recreateEndedSpot(backend, ZLinkPlacementObjectKind.INSTANCE_SPOT, fromClientNode, null);
    }

    @ParameterizedTest
    @CsvSource({"provider, false", "provider, true", "memory, false", "memory, true"})
    void endedEmptyUserGetOrCreateUsesRequestingNodePolicy(String backend, boolean fromClientNode)
            throws Exception {
        recreateEndedSpot(backend, ZLinkPlacementObjectKind.USER_SPOT, fromClientNode, null);
    }

    @ParameterizedTest
    @CsvSource({"provider, false", "provider, true", "memory, false", "memory, true"})
    void userReclamationPolicyComesFromRequestNodeRatherThanFactoryTarget(
            String backend, boolean requestDisabled) throws Exception {
        recreateEndedSpot(backend, ZLinkPlacementObjectKind.USER_SPOT, true, requestDisabled);
    }

    @ParameterizedTest
    @CsvSource({"provider, false", "provider, true", "memory, false", "memory, true"})
    void instanceReclamationPolicyComesFromTargetRatherThanRequestNode(
            String backend, boolean requestDisabled) throws Exception {
        recreateEndedSpot(backend, ZLinkPlacementObjectKind.INSTANCE_SPOT, true, requestDisabled);
    }

    private static void recreateEndedSpot(
            String backend,
            ZLinkPlacementObjectKind kind,
            boolean fromClientNode,
            Boolean requestingNodeDisabled)
            throws Exception {
        ZLinkLocationStore provider =
                backend.equals("memory")
                        ? new ZLinkInMemoryLocationStore()
                        : new ZLinkInMemoryProviderLocationStore();
        // Runtime resolves public LocationStore providers through the provider repository.
        // The internal in-memory repository is exercised separately by EndedSpotRecreationTest.
        ZLinkLocationRepository repository = new ZLinkProviderLocationRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                repository
                                        .claimOwnerLease("ended-instance", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var rid = RoutingId.from("ended-instance");
        var descriptor =
                new ZLinkMeshNodeDescriptor(
                        "mesh",
                        rid,
                        1,
                        1,
                        "tcp://127.0.0.1:7100",
                        Map.of(),
                        1,
                        List.of(
                                new ZLinkObjectCapability(
                                        kind,
                                        "room",
                                        ZLinkObjectMaintenancePolicyKind.DISABLED,
                                        false,
                                        1)),
                        ZLinkMeshNodeObjectRole.SERVER,
                        Optional.of("old-entry-00000000-0000-4000-8000-000000000001"),
                        100,
                        new ZLinkPlacementCapacity(
                                new ZLinkCapacityUsage(0, 0, 1),
                                new ZLinkCapacityUsage(0, 0, 1),
                                List.of()),
                        new ZLinkActivationConcurrency(0, 64),
                        Optional.empty(),
                        systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
                        "security",
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        Instant.now());
        repository
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        String key = ZLinkAuthorityKeyCodec.spot("ended-instance-spot");
        var reservation =
                ((ZLinkObjectReserved)
                                repository
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        kind,
                                                        key,
                                                        "room",
                                                        "intent",
                                                        new byte[32],
                                                        1,
                                                        new ZLinkMeshNodeDescriptorKey("mesh", rid),
                                                        1,
                                                        owner,
                                                        new byte[] {1},
                                                        ZLinkPlacementCapacityBundle.spot(
                                                                kind, "room", 1),
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        byte[] ready =
                kind == ZLinkPlacementObjectKind.USER_SPOT
                        ? new ZLinkServiceAuthorityPayloadCodec()
                                .encodeUser(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        "room",
                                        "ended-instance-spot",
                                        owner.ownerId(),
                                        owner.leaseGeneration(),
                                        "mesh",
                                        rid,
                                        1)
                        : new ZLinkServiceAuthorityPayloadCodec()
                                .encodeInstance(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        "room",
                                        "ended-instance-spot",
                                        owner.ownerId(),
                                        owner.leaseGeneration(),
                                        "mesh",
                                        rid,
                                        1);
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository.commit(reservation, ready, () -> false).toCompletableFuture().join());
        repository.releaseOwnerLease(owner).toCompletableFuture().join();
        var endedAuthority =
                (ZLinkAuthoritySnapshot)
                        repository.read(key, () -> false).toCompletableFuture().join();
        var observed =
                new SteadyInstanceProvider(
                        provider, repository, key, reservation.objectGeneration());
        var options = new DefaultZLinkFrameworkOptions();
        var runtimeStore = observed;
        options.addLocationStore(runtimeStore);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        var server =
                options.addRouteMesh("mesh")
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from("new-instance"))
                        .objects()
                        .server()
                        .addEntrySpot(Entry.class);
        if (kind == ZLinkPlacementObjectKind.INSTANCE_SPOT)
            server.addInstanceSpotFactory(
                    "room",
                    Instance.class,
                    factory -> {
                        if (Boolean.TRUE.equals(requestingNodeDisabled))
                            factory.recreateOnRelocation();
                        else factory.disableRelocation();
                    });
        else
            server.addSpotFactory(
                    "room",
                    User.class,
                    factory -> {
                        if (Boolean.TRUE.equals(requestingNodeDisabled))
                            factory.recreateOnRelocation();
                        else factory.disableRelocation();
                    });
        factories.set(0);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);
            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions.addLocationStore(runtimeStore);
            sourceOptions.addRelocationStore(new InMemoryRelocationStore());
            var sourceMesh =
                    sourceOptions
                            .addRouteMesh("mesh")
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(RoutingId.from("instance-client"));
            if (requestingNodeDisabled == null) sourceMesh.objects().client();
            else {
                sourceMesh.setPlacementWeight(0);
                var sourceServer = sourceMesh.objects().server().addEntrySpot(Entry.class);
                if (kind == ZLinkPlacementObjectKind.INSTANCE_SPOT)
                    sourceServer.addInstanceSpotFactory(
                            "room",
                            Instance.class,
                            factory -> {
                                if (requestingNodeDisabled) factory.disableRelocation();
                                else factory.recreateOnRelocation();
                            });
                else
                    sourceServer.addSpotFactory(
                            "room",
                            User.class,
                            factory -> {
                                if (requestingNodeDisabled) factory.disableRelocation();
                                else factory.recreateOnRelocation();
                            });
            }
            try (var client =
                    fromClientNode ? ZLinkFrameworkRuntimeTestAccess.start(sourceOptions) : null) {
                if (client != null)
                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(client)
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
                if (kind == ZLinkPlacementObjectKind.USER_SPOT) {
                    var creation =
                            (client == null ? runtime : client)
                                    .spotManager()
                                    .getOrCreate("ended-instance-spot", "room")
                                    .submit()
                                    .toCompletableFuture();
                    if (fromClientNode && !Boolean.TRUE.equals(requestingNodeDisabled)) {
                        var failure =
                                assertThrows(
                                        java.util.concurrent.ExecutionException.class,
                                        () -> creation.get(10, TimeUnit.SECONDS));
                        assertEquals(
                                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                                ((systems.zlink.framework.errors.ZLinkFrameworkException)
                                                failure.getCause())
                                        .kind());
                        assertEquals(0, factories.get());
                        assertEquals(
                                reservation.objectGeneration(),
                                ((ZLinkAuthoritySnapshot)
                                                repository
                                                        .read(key, () -> false)
                                                        .toCompletableFuture()
                                                        .join())
                                        .objectGeneration());
                    } else {
                        creation.get(10, TimeUnit.SECONDS);
                        var current = observed.ready.get(10, TimeUnit.SECONDS);
                        assertTrue(current.objectGeneration() > reservation.objectGeneration());
                        assertEquals(1, factories.get());
                    }
                    return;
                }
                var replyOperation =
                        (client == null ? runtime : client)
                                .route()
                                .requestToSpot("ended-instance-spot", new Probe("hello"))
                                .instanceSpot("room")
                                .inMesh("mesh")
                                .timeout(Duration.ofSeconds(10))
                                .submit(Reply.class)
                                .toCompletableFuture();
                if (Boolean.TRUE.equals(requestingNodeDisabled)) {
                    var failure =
                            assertThrows(
                                    java.util.concurrent.ExecutionException.class,
                                    () -> replyOperation.get(10, TimeUnit.SECONDS));
                    assertEquals(
                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                            ((systems.zlink.framework.errors.ZLinkFrameworkException)
                                            failure.getCause())
                                    .kind());
                    assertEquals(0, factories.get());
                    assertEquals(
                            endedAuthority.storeVersion(),
                            ((ZLinkAuthoritySnapshot)
                                            repository
                                                    .read(key, () -> false)
                                                    .toCompletableFuture()
                                                    .join())
                                    .storeVersion());
                    return;
                }
                var reply = replyOperation.get(10, TimeUnit.SECONDS);
                assertTrue(reply.generation() > reservation.objectGeneration());
                assertEquals(1, factories.get());
                // The reply precedes the target's durable first-operation cleanup.
                var current = observed.ready.toCompletableFuture().get(10, TimeUnit.SECONDS);
                assertEquals(reply.generation(), current.objectGeneration());
                assertEquals(
                        RoutingId.from("new-instance"), current.allocation().descriptor().rid());
                assertTrue(
                        new ZLinkServiceAuthorityPayloadCodec()
                                .decode(current.payload())
                                .orElseThrow()
                                .activationRecoveryState()
                                .isEmpty());
            }
        }
    }

    public record Probe(String value) {}

    public record Reply(long generation) {}

    private static final class SteadyInstanceProvider implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final ZLinkLocationRepository repository;
        private final String key;
        private final long oldGeneration;
        private final CompletableFuture<ZLinkAuthoritySnapshot> ready = new CompletableFuture<>();

        private SteadyInstanceProvider(
                ZLinkLocationStore delegate,
                ZLinkLocationRepository repository,
                String key,
                long oldGeneration) {
            this.delegate = delegate;
            this.repository = repository;
            this.key = key;
            this.oldGeneration = oldGeneration;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation)
                    .thenCompose(
                            result ->
                                    repository
                                            .read(key, () -> false)
                                            .thenApply(
                                                    read -> {
                                                        if (read
                                                                        instanceof
                                                                        ZLinkAuthoritySnapshot
                                                                                snapshot
                                                                && snapshot.objectGeneration()
                                                                        > oldGeneration
                                                                && new ZLinkServiceAuthorityPayloadCodec()
                                                                        .decode(snapshot.payload())
                                                                        .filter(
                                                                                authority ->
                                                                                        authority
                                                                                                                .state()
                                                                                                        == ZLinkServiceAuthorityPayloadCodec
                                                                                                                .State
                                                                                                                .READY
                                                                                                && authority
                                                                                                        .activationRecoveryState()
                                                                                                        .isEmpty())
                                                                        .isPresent()) {
                                                            ready.complete(snapshot);
                                                        }
                                                        return result;
                                                    }));
        }
    }

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
            factories.incrementAndGet();
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(Handler.class);
        }
    }

    public static final class User implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public User(ZLinkSpotContext context) {
            this.context = context;
            factories.incrementAndGet();
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Handler implements ZLinkSpotRequestHandler<Instance, Probe, Reply> {
        public CompletionStage<Reply> handle(Instance instance, Probe probe) {
            return CompletableFuture.completedFuture(
                    new Reply(instance.context.objectGeneration()));
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
}
