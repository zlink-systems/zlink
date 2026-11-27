package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * A Join source whose settlement fails (for example {@code Unavailable} at the host stop deadline,
 * host relocation flow section 14) releases its ledger entry: the cleanup waiters end with that
 * failure, the entry is gone, and the unit makes no further Store call. The test-side {@code
 * orTimeout} only bounds the future under assertion; it is not a product timeout.
 */
final class ZLinkActorJoinFailedSettlementLedgerTest {
    private static final String MESH = "failed-settlement-ledger";
    private static final String ACTOR_ID = "actor-ledger";
    private static final String ACTOR_TYPE = "player";
    private static final String SPOT_TYPE = "room";
    private static final String TARGET_SPOT_ID = "room-ledger";
    private static final RoutingId SOURCE_RID = RoutingId.from("ledger-source");
    private static final RoutingId TARGET_RID = RoutingId.from("ledger-target");
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final long WAIT_SECONDS = 10;
    private static final systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation
            OPEN = () -> false;

    @Test
    void failedSettlementReleasesTheSourceLedgerWithoutFurtherStoreCalls() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var targetOptions = options(TARGET_RID, "inproc://ledger-target", locations, relocations);
        var sourceOptions = options(SOURCE_RID, "inproc://ledger-source", locations, relocations);
        try (ZLinkFrameworkRuntime targetHost =
                        ZLinkFrameworkRuntimeTestAccess.start(targetOptions);
                ZLinkFrameworkRuntime sourceHost =
                        ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(targetHost)
                    .toCompletableFuture()
                    .get();
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(sourceHost)
                    .toCompletableFuture()
                    .get();
            var sourceSpots = ZLinkFrameworkRuntimeTestAccess.spots(sourceHost);
            var sourceActors = ZLinkFrameworkRuntimeTestAccess.actors(sourceHost);
            targetHost
                    .spotManager()
                    .getOrCreate(TARGET_SPOT_ID, SPOT_TYPE)
                    .submit()
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    sourceHost
                            .actorManager()
                            .create(ACTOR_ID, ACTOR_TYPE)
                            .submit()
                            .toCompletableFuture()
                            .get());

            var repository = new ZLinkProviderLocationRepository(locations);
            var storeCalls = new AtomicInteger();
            ZLinkLocationRepository observed = counting(repository, storeCalls);
            var coordinator = new ZLinkAggregateRelocationCoordinator(observed);
            ZLinkMeshNodeDescriptor sourceDescriptor = descriptor(repository, SOURCE_RID);
            ZLinkMeshNodeDescriptor targetDescriptor = descriptor(repository, TARGET_RID);
            ZLinkAuthoritySnapshot targetSpotAuthority =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository
                                    .read(ZLinkAuthorityKeyCodec.spot(TARGET_SPOT_ID), OPEN)
                                    .toCompletableFuture()
                                    .get());
            var node = inertNode();
            var adapters =
                    new ZLinkRelocationAdapterRegistry(
                            sourceOptions.registration(), ZLinkHandlerActivator.reflection());
            var builder =
                    new ZLinkStandaloneActorRelocationSourceBuilder(
                            MESH,
                            SOURCE_RID,
                            sourceDescriptor.lifecycleGeneration(),
                            observed,
                            systems.zlink.framework.testing.ZLinkDescriptorLeaseTestFixture
                                    .resolver(observed),
                            coordinator,
                            sourceSpots.actorSessions(),
                            adapters,
                            sourceOptions
                                    .registration()
                                    .meshNodes()
                                    .getFirst()
                                    .relocatableActorFactories(),
                            sourceSpots,
                            new ZLinkSessionRelocationPeerClient(node),
                            TIMEOUT);
            var settlement = new CompletableFuture<ZLinkRelocationTransitionClient.Settlement>();
            var join = new ZLinkActorJoinCanonicalAdapter(sourceActors, sourceSpots);
            join.register(SOURCE_RID, node, builder, new HeldSettlement(settlement));

            ZLinkActor actor = sourceActors.localActor(ACTOR_ID).orElseThrow();
            var sourceRef = sourceActors.currentRef(actor);
            var goal =
                    new ZLinkActorJoinRelocationPort.Goal(
                            UUID.randomUUID(),
                            new ZLinkActorJoinOperationId(0x1111L, 0x2222L),
                            sourceRef,
                            ACTOR_TYPE,
                            TARGET_SPOT_ID,
                            targetSpotAuthority.objectGeneration(),
                            TARGET_RID,
                            targetDescriptor.lifecycleGeneration(),
                            targetSpotAuthority.authorityOwnerGeneration(),
                            targetDescriptor.leaseGeneration(),
                            new byte[] {9, 4},
                            null,
                            0L,
                            "application/json",
                            new byte[0],
                            "application/x-zlink-multipart");

            var submission =
                    join.relocate(goal, TIMEOUT)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var cleanup = join.awaitSourceCleanup(ACTOR_ID, sourceRef.generation());
            assertFalse(cleanup.toCompletableFuture().isDone(), "settlement is still pending");
            assertEquals(1, join.sourceAttemptCountForTest());

            int callsAtFailure = storeCalls.get();
            settlement.completeExceptionally(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "host stop deadline reached before the unit settled"));

            //  The waiter ends exceptionally with the settlement failure: the unit did not
            //  commit, so no source leave is owed. A hang here is the ledger leak.
            var waiter = cleanup.toCompletableFuture().orTimeout(WAIT_SECONDS, TimeUnit.SECONDS);
            var failure = assertThrows(ExecutionException.class, waiter::get);
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertTrue(submission.sourceCleanup().toCompletableFuture().isCompletedExceptionally());

            assertEquals(0, join.sourceAttemptCountForTest(), "the ledger entry is removed");
            var again = join.awaitSourceCleanup(ACTOR_ID, sourceRef.generation());
            assertTrue(
                    again.toCompletableFuture().isDone()
                            && !again.toCompletableFuture().isCompletedExceptionally(),
                    "a later waiter for the same Actor returns immediately");
            assertEquals(callsAtFailure, storeCalls.get(), "no Store call after the failure");
        }
    }

    private record HeldSettlement(
            CompletableFuture<ZLinkRelocationTransitionClient.Settlement> settlement)
            implements ZLinkRelocationTransitionClient {
        @Override
        public CompletionStage<Void> stage(
                RoutingId targetNodeRid,
                ZLinkSpotRetireControl.StageRequest request,
                Duration timeout,
                Instant deadline) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> relay(
                RoutingId targetNodeRid,
                ZLinkSpotRetireControl.Fence fence,
                byte[] frozenRecord,
                Duration timeout) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publish(
                RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Settlement> settle(
                RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Instant preserveAt) {
            return settlement;
        }

        @Override
        public CompletionStage<Void> abort(
                RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout) {
            return CompletableFuture.completedFuture(null);
        }
    }

    /** The adapter only installs its ActorLeft handler on this node; nothing else may call it. */
    private static ZLinkInternalMeshNode inertNode() {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("setActorLeftHandler")) return null;
                            if (method.getDeclaringClass() == Object.class)
                                return method.invoke(proxy, arguments);
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static ZLinkLocationRepository counting(
            ZLinkLocationRepository delegate, AtomicInteger calls) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            calls.incrementAndGet();
                            try {
                                return method.invoke(delegate, arguments);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationRepository repository, RoutingId rid) {
        return repository
                .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                .toCompletableFuture()
                .join()
                .items()
                .stream()
                .filter(value -> value.rid().equals(rid))
                .findFirst()
                .orElseThrow();
    }

    private static DefaultZLinkFrameworkOptions options(
            RoutingId rid,
            String endpoint,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.registration().locations().options().setRouteCacheMaxAge(Duration.ZERO);
        var mesh = options.addRouteMesh(MESH).setRoutingId(rid).listen(endpoint);
        mesh.channelName(MESH).server();
        mesh.objects()
                .server()
                .addEntrySpot(LedgerEntrySpot.class)
                .addSpotFactory(SPOT_TYPE, LedgerSpot.class, factory -> factory.disableRelocation())
                .addActorFactory(
                        ACTOR_TYPE,
                        LedgerActor.class,
                        LedgerActorFactory.class,
                        factory -> factory.preserveStateWith(LedgerAdapter.class));
        options.validate();
        return options;
    }

    public static final class LedgerEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
        private final ZLinkEntrySpotContext context;

        public LedgerEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
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

    public static final class LedgerSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public LedgerSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
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

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class LedgerActor implements ZLinkActor {
        private final ZLinkActorContext context;

        public LedgerActor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class LedgerActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new LedgerActor(context));
        }
    }

    public static final class LedgerAdapter implements ZLinkActorRelocationAdapter<LedgerActor> {
        @Override
        public CompletionStage<byte[]> capture(
                LedgerActor actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[] {7, 2, 6});
        }

        @Override
        public CompletionStage<Void> restore(
                LedgerActor actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
