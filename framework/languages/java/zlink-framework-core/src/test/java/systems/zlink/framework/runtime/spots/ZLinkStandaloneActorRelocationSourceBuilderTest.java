package systems.zlink.framework.runtime.spots;
import systems.zlink.framework.locationprovider.ZLinkRelocationStore;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

final class ZLinkStandaloneActorRelocationSourceBuilderTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;
    private static final String MESH = "actor-retire-source";
    private static final String ACTOR_TYPE = "player";
    private static final RoutingId SOURCE_RID =
        RoutingId.from("actor-source");
    private static final RoutingId TARGET_RID =
        RoutingId.from("actor-target");

    @Test
    void capturesEntryActorAndRestoresSourceQueueAfterPrecommitAbort()
        throws Exception {
        SnapshotAdapter.captured.set(null);
        var locations = new ZLinkInMemoryLocationStore();
        var repository = new ZLinkProviderLocationRepository(locations);
        var relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(
            locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime =
                (ZLinkSpotRuntime) host.spotManager();
            var created = host.actorManager()
                .create("actor-a", ACTOR_TYPE)
                .submit()
                .toCompletableFuture().get();
            assertInstanceOf(ZLinkActorCreateResult.Created.class, created);
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH, ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().stream()
                .filter(value -> value.rid().equals(
                    nodeRegistration.routingId()))
                .findFirst().orElseThrow();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                        "actor-target-owner", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            repository.updateMeshNode(
                    descriptor(targetOwner),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            var coordinator = new ZLinkAggregateRelocationCoordinator(
                repository, relocations);
            var builder = new ZLinkStandaloneActorRelocationSourceBuilder(
                MESH,
                nodeRegistration.routingId(),
                source.lifecycleGeneration(),
                repository,
                coordinator,
                runtime.actorSessions(),
                new ZLinkRelocationAdapterRegistry(
                    registration,
                    ZLinkHandlerActivator.reflection()),
                nodeRegistration.relocatableActorFactories(),
                runtime,
                null);

            var prepared = builder.prepare(
                    "actor-a",
                    rollingToVersionOne(),
                    NEVER)
                .toCompletableFuture().get();

            assertNotNull(SnapshotAdapter.captured.get());
            assertTrue(runtime.actorSessions()
                .localActor("actor-a").isPresent());
            var root = coordinator.readRoot(
                    prepared.initialRoot().stored().reference(),
                    prepared.initialRoot().stored().checksumCrc32c(),
                    NEVER)
                .toCompletableFuture().get();
            var decoded = ZLinkCanonicalActorRelocationEnvelope.decode(
                root.payload(),
                prepared.targetRequest().relocationId(),
                "actor-a",
                true);
            assertArrayEquals(new byte[] {7, 2, 6}, decoded.state());
            assertEquals(
                prepared.targetRequest().objectGeneration(),
                decoded.objectGeneration());

            prepared.abort().toCompletableFuture().get();

            assertTrue(runtime.actorSessions()
                .localActor("actor-a").isPresent());
        }
    }

    @Test
    void productionTargetStagesPostCutRequestUntilPhase4AndReplaysAfterAdmission()
        throws Exception {
        SnapshotAdapter.captured.set(null);
        var locations = new ZLinkInMemoryLocationStore();
        var repository = new ZLinkProviderLocationRepository(locations);
        var relocations = new InMemoryRelocationStore();
        DefaultZLinkFrameworkOptions options = options(
            locations, relocations);
        var registration = options.registration();
        var nodeRegistration = registration.meshNodes().getFirst();
        try (ZLinkFrameworkRuntime host =
                ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkSpotRuntime runtime =
                (ZLinkSpotRuntime) host.spotManager();
            assertInstanceOf(
                ZLinkActorCreateResult.Created.class,
                host.actorManager().create("actor-b", ACTOR_TYPE)
                    .submit()
                    .toCompletableFuture().get());
            ZLinkMeshNodeDescriptor source = repository.listMeshNodes(
                    MESH, ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items().getFirst();
            ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                repository.claimOwnerLease(
                        "actor-target-owner", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            repository.updateMeshNode(
                    descriptor(targetOwner),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            var coordinator = new ZLinkAggregateRelocationCoordinator(
                repository, relocations);
            var adapters = new ZLinkRelocationAdapterRegistry(
                registration,
                ZLinkHandlerActivator.reflection());
            var builder = new ZLinkStandaloneActorRelocationSourceBuilder(
                MESH,
                nodeRegistration.routingId(),
                source.lifecycleGeneration(),
                repository,
                coordinator,
                runtime.actorSessions(),
                adapters,
                nodeRegistration.relocatableActorFactories(),
                runtime,
                null);
            var prepared = builder.prepare(
                    "actor-b",
                    rollingToVersionOne(),
                    NEVER)
                .toCompletableFuture().get();
            long objectGeneration =
                prepared.targetRequest().objectGeneration();
            long sourceOwnerGeneration =
                prepared.targetRequest().sourceAuthorityOwnerGeneration();
            ActorTargetBackend targetBackend = new ActorTargetBackend();
            AtomicInteger targetReplayed = new AtomicInteger();
            var actorStaging =
                new ZLinkStandaloneActorRelocationStagingOwner(targetBackend);
            AtomicReference<ZLinkCanonicalRelocationStateMachine>
                sourceMachine = new AtomicReference<>();
            AtomicReference<ZLinkCanonicalRelocationStateMachine>
                targetMachine = new AtomicReference<>();
            ZLinkInternalMeshNode sourceTransport = canonicalTransport(
                nodeRegistration.routingId(),
                source.lifecycleGeneration(),
                targetMachine);
            ZLinkInternalMeshNode targetTransport = canonicalTransport(
                TARGET_RID,
                9,
                sourceMachine);
            var endpoint = new ZLinkUserSpotRetireTargetEndpoint(
                TARGET_RID,
                9,
                coordinator,
                unusedSpotStaging(),
                ignored -> null,
                (lane, record) -> {
                    assertEquals("actor:actor-b", lane);
                    assertTrue(
                        ZLinkActorAcceptedJournal.decode(record.payload())
                            .header().packetName().startsWith("post-freeze"));
                    targetReplayed.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                },
                null,
                Duration.ZERO,
                request -> coordinator.normalizePublishedAggregate(
                    request.participants().stream()
                        .map(value -> new ZLinkAggregateRelocationCoordinator
                            .ExpectedParticipant(
                                value.authorityKey(),
                                value.objectGeneration(),
                                value.sourceAuthorityOwnerGeneration()))
                        .toList(),
                    new ZLinkAggregateFence(
                        request.fence().aggregateId(),
                        request.fence().aggregateGeneration()),
                    new ZLinkLocationOwnerToken(
                        request.targetOwnerId(),
                        request.targetOwnerLeaseGeneration()),
                    NEVER),
                null,
                ZLinkSpotRetireControl.client(targetTransport),
                repository,
                actorStaging);
            sourceMachine.set(new ZLinkCanonicalRelocationStateMachine(
                sourceTransport,
                MESH,
                nodeRegistration.entrySpotId(),
                repository,
                coordinator,
                unusedRelocationTarget()));
            targetMachine.set(new ZLinkCanonicalRelocationStateMachine(
                targetTransport,
                MESH,
                "actor-target-entry-00000000-0000-4000-8000-000000000001",
                repository,
                coordinator,
                endpoint));
            Duration timeout = Duration.ofSeconds(5);
            sourceMachine.get().stage(
                    TARGET_RID, prepared.stageRequest(), timeout)
                .toCompletableFuture().get();
            List<CompletableFuture<Void>> accepted = new java.util.ArrayList<>();
            AtomicInteger released = new AtomicInteger();
            for (int index = 0; index < 1025; index++) {
                byte[] suffixRecord = ZLinkAcceptedJournalTestRecords.actor(
                    "actor-b",
                    0,
                    "post-freeze-" + index,
                    Map.of(),
                    new byte[] {(byte) index});
                accepted.add(runtime.actorSessions()
                    .actorRelocationLane("actor-b")
                    .enqueueRelocatable(
                        suffixRecord,
                        () -> fail(
                            "source must not execute transferred ingress"),
                        released::incrementAndGet)
                    .toCompletableFuture());
            }
            byte[] lateRecord = ZLinkAcceptedJournalTestRecords.actor(
                "actor-b", 0, "post-freeze-late", Map.of(), new byte[] {10});
            AtomicBoolean lateReleased = new AtomicBoolean();
            CompletableFuture<Void> lateAccepted = runtime.actorSessions()
                .actorRelocationLane("actor-b")
                .enqueueRelocatable(
                    lateRecord,
                    () -> fail("late source ingress must remain held"),
                    () -> lateReleased.set(true))
                .toCompletableFuture();
            assertTrue(accepted.stream().noneMatch(CompletableFuture::isDone));
            assertFalse(lateAccepted.isDone());
            assertEquals(0, released.get());
            assertFalse(lateReleased.get());
            prepared.relayCapturedIngress(sourceMachine.get(), timeout)
                .toCompletableFuture().get();

            byte[] stagedTargetRecord = ZLinkAcceptedJournalTestRecords.actor(
                "actor-b", 41, "target-staged", Map.of(), new byte[] {11});
            AtomicInteger targetIngressFailures = new AtomicInteger();
            List<String> targetIngressReplies =
                new java.util.concurrent.CopyOnWriteArrayList<>();
            assertTrue(endpoint.handleActor(
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                    nodeRegistration.routingId(),
                    source.lifecycleGeneration(),
                    source.ownerId(),
                    source.leaseGeneration()),
                new ZLinkServiceM6BWireCodec.ActorMessage(
                    true,
                    0,
                    41L,
                    211,
                    223,
                    1,
                    null,
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorRef(
                                TARGET_RID, "actor-b", objectGeneration),
                        9,
                        sourceOwnerGeneration + 1,
                        targetOwner.leaseGeneration())),
                () -> stagedTargetRecord,
                List.of(Message.from("target-staged-payload")),
                null,
                reply -> {
                    targetIngressReplies.add(
                        reply.getFirst().toUtf8String());
                    reply.forEach(Message::close);
                },
                ignored -> targetIngressFailures.incrementAndGet()));
            assertEquals(0, targetIngressFailures.get());
            assertTrue(targetIngressReplies.isEmpty(),
                "the request must stay staged before target publish");

            sourceMachine.get().publish(
                    TARGET_RID, prepared.stageRequest().fence(), timeout)
                .toCompletableFuture().get();
            assertEquals(0, targetReplayed.get(),
                "standalone Actor records must replay through the Actor staging owner");
            assertEquals(1027, targetBackend.replayedPackets.size());
            for (int index = 0; index < 1_025; index++) {
                assertEquals("post-freeze-" + index,
                    targetBackend.replayedPackets.get(index));
            }
            assertEquals("post-freeze-late",
                targetBackend.replayedPackets.get(1_025));
            assertEquals("target-staged",
                targetBackend.replayedPackets.getLast());
            assertEquals(List.of("target-staged-reply"),
                targetIngressReplies);
            prepared.completeSourceQueueCommit();
            CompletableFuture.allOf(
                    accepted.toArray(CompletableFuture[]::new))
                .get(3, TimeUnit.SECONDS);
            lateAccepted.get();
            assertEquals(1025, released.get());
            assertTrue(lateReleased.get());
            prepared.cleanupLocal().toCompletableFuture().get();
            prepared.discardInitialAfterCommit().toCompletableFuture().get();

            assertTrue(targetBackend.published.get());
            assertTrue(targetBackend.admitted.get());
            assertEquals(1_027, targetBackend.admittedTurns.get());
            assertEquals(1, targetBackend.maxActiveTurns.get());
            ZLinkAuthoritySnapshot authority = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository.read(
                        ZLinkAuthorityKeyCodec.actor("actor-b"), NEVER)
                    .toCompletableFuture().get());
            assertEquals(objectGeneration, authority.objectGeneration());
            assertEquals(
                sourceOwnerGeneration + 1,
                authority.authorityOwnerGeneration());
        }
    }

    private static DefaultZLinkFrameworkOptions options(
        ZLinkInMemoryLocationStore locations,
        ZLinkRelocationStore relocations) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        var mesh = options.addRouteMesh(MESH)
            .setRoutingIdPrefix(SOURCE_RID.toString())
            .listen("inproc://actor-retire-source");
        mesh.channelName(MESH).server();
        mesh.objects().server().addActorFactory(
            ACTOR_TYPE,
            TestActor.class,
            TestActorFactory.class,
            factory -> factory.preserveStateWith(SnapshotAdapter.class));
        options.validate();
        return options;
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkLocationOwnerToken owner) {
        return new ZLinkMeshNodeDescriptor(
            MESH,
            TARGET_RID,
            9,
            1,
            "inproc://actor-retire-target",
            Map.of(MESH, 100),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                ACTOR_TYPE,
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(
                "actor-target-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 64),
                new ZLinkCapacityUsage(0, 0, 64),
                List.of()),
            new ZLinkActivationConcurrency(0, 64),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static ZLinkRelocationTargetPolicy rollingToVersionOne() {
        return new ZLinkRelocationTargetPolicy(
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
            0,
            Optional.empty(),
            1);
    }

    private static ZLinkUserSpotAggregateStagingOwner unusedSpotStaging() {
        return new ZLinkUserSpotAggregateStagingOwner(
            new ZLinkUserSpotAggregateStagingOwner.StagingBackend() {
                private AssertionError unused() {
                    return new AssertionError(
                        "User Spot staging must not handle an Actor root");
                }

                @Override public CompletionStage<Object> prepareSpot(
                    ZLinkUserSpotAggregateStagingOwner.Request request) {
                    return CompletableFuture.failedFuture(unused());
                }

                @Override public CompletionStage<Void> restoreSpot(
                    Object spot,
                    ZLinkUserSpotAggregateStagingOwner.Request request,
                    ZLinkRelocationCancellation cancellation) {
                    return CompletableFuture.failedFuture(unused());
                }

                @Override public CompletionStage<Object> prepareActor(
                    ZLinkUserSpotAggregateStagingOwner.ActorParticipant actor,
                    ZLinkRelocationCancellation cancellation) {
                    return CompletableFuture.failedFuture(unused());
                }

                @Override public void publishSpot(Object spot) {
                    throw unused();
                }

                @Override public void publishActor(Object actor) {
                    throw unused();
                }

                @Override public void completeActor(Object actor) {
                    throw unused();
                }

                @Override public void publishTimers(Object spot) {
                    throw unused();
                }

                @Override public CompletionStage<Void> discardActor(
                    Object actor) {
                    return CompletableFuture.failedFuture(unused());
                }

                @Override public void discardSpot(Object spot) {
                    throw unused();
                }
            });
    }

    private static ZLinkInternalMeshNode canonicalTransport(
        RoutingId localRid,
        long generation,
        AtomicReference<ZLinkCanonicalRelocationStateMachine> peer) {
        var status = new systems.zlink.framework.runtime.internal.binding.spot
            .MeshNodeStatus(
                systems.zlink.framework.runtime.internal.binding.spot
                    .MeshNodeState.READY,
                localRid,
                MESH,
                "",
                generation,
                1,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                return switch (method.getName()) {
                    case "status" -> status;
                    case "sendCanonicalRelocationControl" ->
                        peer.get().apply(
                            localRid,
                            Byte.toUnsignedInt(
                                ((byte[]) arguments[1])[3]),
                            ((byte[]) arguments[1]).clone());
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                };
            });
    }

    private static ZLinkSpotRetireControl.TargetEndpoint
        unusedRelocationTarget() {
        return new ZLinkSpotRetireControl.TargetEndpoint() {
            private CompletionStage<Void> unused() {
                return CompletableFuture.failedFuture(
                    new AssertionError(
                        "source owner must not run target relocation"));
            }

            @Override public CompletionStage<Void> stage(
                ZLinkSpotRetireControl.StageRequest request) {
                return unused();
            }

            @Override public CompletionStage<Void> publish(
                ZLinkSpotRetireControl.StageRequest request) {
                return unused();
            }

            @Override public CompletionStage<Void> abort(
                ZLinkSpotRetireControl.StageRequest request) {
                return unused();
            }

        };
    }

    private static final class ActorTargetBackend
        implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final AtomicBoolean published = new AtomicBoolean();
        private final AtomicBoolean admitted = new AtomicBoolean();
        private final List<String> replayedPackets =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        private final AtomicInteger admittedTurns = new AtomicInteger();
        private final AtomicInteger activeTurns = new AtomicInteger();
        private final AtomicInteger maxActiveTurns = new AtomicInteger();

        @Override public CompletionStage<Object> prepare(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            assertArrayEquals(new byte[] {7, 2, 6}, state);
            return CompletableFuture.completedFuture(new Object());
        }

        @Override public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            ZLinkActorAcceptedJournal.Record record) {
            assertTrue(published.get());
            assertTrue(admitted.get(),
                "durable Actor backlog ran before lifecycle admission");
            replayedPackets.add(record.header().packetName());
            return CompletableFuture.completedFuture(
                record.header().packetName().equals("target-staged")
                    ? Optional.of("target-staged-reply".getBytes())
                    : Optional.empty());
        }

        @Override public <T> CompletionStage<T> admitApplicationJob(
            Supplier<CompletionStage<T>> turn) {
            admittedTurns.incrementAndGet();
            int active = activeTurns.incrementAndGet();
            maxActiveTurns.accumulateAndGet(active, Math::max);
            CompletionStage<T> result;
            try {
                result = turn.get();
            } catch (RuntimeException | Error failure) {
                activeTurns.decrementAndGet();
                throw failure;
            }
            return result.whenComplete(
                (ignored, failure) -> activeTurns.decrementAndGet());
        }

        @Override public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            assertTrue(published.compareAndSet(false, true));
        }

        @Override public void openAdmission(Object actor) {
            assertTrue(published.get());
            assertTrue(replayedPackets.isEmpty(),
                "durable backlog must not run before lifecycle admission");
            assertTrue(admitted.compareAndSet(false, true));
        }

        @Override public CompletionStage<Void> discard(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestActor implements ZLinkActor {
        private final ZLinkActorContext context;

        public TestActor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new TestActor(context));
        }
    }

    public static final class SnapshotAdapter
        implements ZLinkActorRelocationAdapter<TestActor> {
        private static final AtomicReference<TestActor> captured =
            new AtomicReference<>();

        @Override
        public CompletionStage<byte[]> capture(
            TestActor actor,
            ZLinkRelocationCancellation cancellation) {
            captured.set(actor);
            return CompletableFuture.completedFuture(
                new byte[] {7, 2, 6});
        }

        @Override
        public CompletionStage<Void> restore(
            TestActor actor,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
