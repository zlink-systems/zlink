package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotAggregateStagingOwner.StagingBackend;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Collectors;

final class ZLinkSpotRetireSessionRouteHandoffTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;
    private static final String MESH = "retire-session-route";
    private static final String SPOT_ID = "forest-2";
    private static final String SPOT_TYPE = "room";
    private static final String ACTOR_TYPE = "player";
    private static final String FIRST_ACTOR_ID = "z";
    private static final String SECOND_ACTOR_ID = "aa";
    private static final RoutingId TARGET_RID = RoutingId.from("target-node");

    @AfterEach
    void clearTestState() {
        TestSession.connected.clear();
        TestActor.instances.clear();
        TestActor.joinCompletions.clear();
    }

    @RepeatedTest(40)
    void twoSealedActorBindingsFollowParticipantOrderThroughReadyAndCommit() throws Exception {
        ZLinkInMemoryLocationStore locations = new ZLinkInMemoryLocationStore();
        var repository = new ZLinkProviderLocationRepository(locations);
        var options = options(locations);
        var nodeRegistration = options.registration().meshNodes().getFirst();
        var wire = new ZLinkServiceM6BWireCodec();
        List<ZLinkServiceM6BWireCodec.SessionRelocationSealed> seals = new CopyOnWriteArrayList<>();
        List<SubmittedRoute> routes = new CopyOnWriteArrayList<>();
        var peer = peer(wire, seals, routes);
        var sessionPeer = new ZLinkSessionRelocationPeerClient(peer);
        var source = new AtomicReference<ZLinkUserSpotRetireSourceBuilder.PreparedSource>();
        var sourceCompleted = new AtomicReference<>(false);
        var targetCommitted = new AtomicReference<>(false);

        try (ZLinkFrameworkRuntime host = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var spotRuntime = ZLinkFrameworkRuntimeTestAccess.spots(host);
            var sourceDescriptor = descriptor(repository, nodeRegistration.routingId());
            var targetOwner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease("target-owner", Duration.ofSeconds(30))
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .token();
            var targetDescriptor = descriptor(TARGET_RID, 9, targetOwner, "inproc://retire-target");
            repository
                    .updateMeshNode(targetDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);

            host.spotManager()
                    .getOrCreate(SPOT_ID, SPOT_TYPE)
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            ActorRef firstActor = createJoinedActor(host, FIRST_ACTOR_ID);
            ActorRef secondActor = createJoinedActor(host, SECOND_ACTOR_ID);

            URI streamEndpoint =
                    URI.create(
                            host.listenerStatus(
                                            systems.zlink.framework.monitoring.ZLinkListenerKind
                                                    .STREAM,
                                            "gateway")
                                    .endpoint());
            try (Socket firstSocket =
                            new Socket(streamEndpoint.getHost(), streamEndpoint.getPort());
                    Socket secondSocket =
                            new Socket(streamEndpoint.getHost(), streamEndpoint.getPort())) {
                ZLinkSessionContext firstSession = awaitSession();
                ZLinkSessionContext secondSession = awaitSession();
                var firstOwner =
                        host.sessionActors("gateway", firstSession.routingId().orElseThrow());
                var secondOwner =
                        host.sessionActors("gateway", secondSession.routingId().orElseThrow());
                firstOwner.bindOrGet(firstActor).toCompletableFuture().get(5, TimeUnit.SECONDS);
                secondOwner.bindOrGet(secondActor).toCompletableFuture().get(5, TimeUnit.SECONDS);
                long firstBindingGeneration =
                        firstOwner.boundBindingGeneration(FIRST_ACTOR_ID).orElseThrow();
                long secondBindingGeneration =
                        secondOwner.boundBindingGeneration(SECOND_ACTOR_ID).orElseThrow();

                var sourceBuilder =
                        new ZLinkUserSpotRetireSourceBuilder(
                                MESH,
                                sourceDescriptor.rid(),
                                sourceDescriptor.lifecycleGeneration(),
                                repository,
                                systems.zlink.framework.testing.ZLinkDescriptorLeaseTestFixture
                                        .resolver(repository),
                                new ZLinkAggregateRelocationCoordinator(repository),
                                spotRuntime.spotLifecycle(),
                                spotRuntime.actorSessions(),
                                new ZLinkRelocationAdapterRegistry(
                                        options.registration(), ZLinkHandlerActivator.reflection()),
                                nodeRegistration.relocatableSpotFactories(),
                                nodeRegistration.relocatableActorFactories(),
                                spotRuntime,
                                sessionPeer);
                var prepared =
                        sourceBuilder
                                .prepare(
                                        SPOT_ID,
                                        rollingToVersionOne(),
                                        NEVER,
                                        new CompletableFuture<>())
                                .toCompletableFuture()
                                .get(15, TimeUnit.SECONDS);
                source.set(prepared);

                var request = prepared.stageRequest();
                assertEquals(2, request.sessionRoutes().size());
                assertEquals(
                        Set.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID),
                        request.sessionRoutes().stream()
                                .map(ZLinkSpotRetireControl.SessionRouteFence::actorId)
                                .collect(Collectors.toSet()));
                assertEquals(
                        List.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID, SPOT_ID),
                        request.participants().stream()
                                .map(ZLinkSpotRetireControl.ParticipantFence::objectId)
                                .toList());
                assertEquals(2, seals.size());
                assertEquals(
                        Set.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID),
                        seals.stream()
                                .map(value -> value.actor().actor().actorId())
                                .collect(Collectors.toSet()));
                assertEquals(
                        List.of(
                                ServiceWirePilotCodec.SessionSealResult.SEALED,
                                ServiceWirePilotCodec.SessionSealResult.SEALED),
                        seals.stream().map(value -> value.result()).toList());

                var targetCoordinator = new ZLinkAggregateRelocationCoordinator(repository);
                var endpoint =
                        new ZLinkUserSpotRetireTargetEndpoint(
                                TARGET_RID,
                                9,
                                targetCoordinator,
                                new ZLinkUserSpotAggregateStagingOwner(new TestStagingBackend()),
                                ignored -> TestSpot.class,
                                (lane, record) -> CompletableFuture.completedFuture(null),
                                sessionPeer,
                                Duration.ofSeconds(3),
                                ignored -> CompletableFuture.completedFuture(null));
                var controlTarget = ZLinkSpotRetireControl.install(peer, endpoint);
                byte[] ready =
                        controlTarget
                                .handle(
                                        request.sourceNodeRid(),
                                        ZLinkSpotRetireControl.encodeStage(request))
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS);
                assertNotNull(ready, "target stage must return READY");
                assertTrue(ready.length > 0);

                prepared.relayCapturedIngress(noOpTransitionClient(), Duration.ofSeconds(3))
                        .toCompletableFuture()
                        .get(10, TimeUnit.SECONDS);
                var targetRequest =
                        targetRequest(repository, request, targetOwner, targetDescriptor);
                var targetPrepared =
                        targetCoordinator
                                .prepare(targetRequest, NEVER)
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS);
                targetCoordinator
                        .commit(targetPrepared, NEVER)
                        .toCompletableFuture()
                        .get(10, TimeUnit.SECONDS);
                targetCommitted.set(true);

                byte[] committed =
                        controlTarget
                                .handle(
                                        request.sourceNodeRid(),
                                        ZLinkSpotRetireControl.encodeFence(2, request.fence()))
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS);
                assertNotNull(committed, "target commit must complete");
                assertTrue(committed.length > 0);
                assertEquals(2, routes.size());
                assertEquals(
                        Set.of(FIRST_ACTOR_ID, SECOND_ACTOR_ID),
                        routes.stream()
                                .map(value -> value.command().actor().actorId())
                                .collect(Collectors.toSet()));

                var routeFences = request.sessionRoutes();
                for (int index = 0; index < routeFences.size(); index++) {
                    var expected = routeFences.get(index);
                    var submitted = routes.get(index);
                    var command = submitted.command();
                    assertEquals(expected.sessionOwnerNodeRid(), submitted.destination());
                    assertEquals(expected.actorId(), command.actor().actorId());
                    assertEquals(expected.actorObjectGeneration(), command.actor().generation());
                    assertEquals(expected.sessionRid(), command.session().sessionRid());
                    assertEquals(
                            expected.bindingGeneration(), command.session().bindingGeneration());
                    assertEquals(
                            expected.sessionOwnerNodeGeneration(),
                            command.session().nodeGeneration());
                    assertEquals(expected.sessionOwnerId(), command.session().ownerId());
                    assertEquals(
                            expected.sessionOwnerLeaseGeneration(),
                            command.session().ownerLeaseGeneration());
                    assertEquals(
                            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
                            command.action());
                }
                var routesByActorId =
                        routes.stream()
                                .collect(
                                        Collectors.toMap(
                                                value -> value.command().actor().actorId(),
                                                SubmittedRoute::command));
                assertEquals(
                        firstSession.routingId().orElseThrow(),
                        routesByActorId.get(FIRST_ACTOR_ID).session().sessionRid());
                assertEquals(
                        firstBindingGeneration,
                        routesByActorId.get(FIRST_ACTOR_ID).session().bindingGeneration());
                assertEquals(
                        secondSession.routingId().orElseThrow(),
                        routesByActorId.get(SECOND_ACTOR_ID).session().sessionRid());
                assertEquals(
                        secondBindingGeneration,
                        routesByActorId.get(SECOND_ACTOR_ID).session().bindingGeneration());

                prepared.completeSourceBarrierCommit()
                        .toCompletableFuture()
                        .get(10, TimeUnit.SECONDS);
                prepared.discardInitialAfterCommit().toCompletableFuture().get();
                sourceCompleted.set(true);
            } finally {
                var prepared = source.get();
                if (prepared != null && !sourceCompleted.get() && !targetCommitted.get()) {
                    prepared.abortPrecommit().toCompletableFuture().get(10, TimeUnit.SECONDS);
                }
            }
        }
    }

    private static DefaultZLinkFrameworkOptions options(ZLinkInMemoryLocationStore locations) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(new InMemoryRelocationStore());
        var mesh =
                options.addRouteMesh(MESH)
                        .setRoutingIdPrefix("source-node")
                        .listen("inproc://retire-session-route-source");
        mesh.channelName(MESH).server();
        mesh.objects()
                .server()
                .addEntrySpot(TestEntrySpot.class)
                .addSpotFactory(
                        SPOT_TYPE,
                        TestSpot.class,
                        factory -> factory.preserveStateWith(TestSpotAdapter.class))
                .addActorFactory(
                        ACTOR_TYPE,
                        TestActor.class,
                        TestActorFactory.class,
                        factory -> factory.recreateOnRelocation());
        options.addStreamNode("gateway")
                .bind("tcp://127.0.0.1:0")
                .enableActorDispatch()
                .registerSession(TestSession.class);
        options.validate();
        return options;
    }

    private static ActorRef createJoinedActor(ZLinkFrameworkRuntime host, String actorId)
            throws Exception {
        var created =
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        host.actorManager()
                                .create(actorId, ACTOR_TYPE)
                                .submit()
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS));
        var completion = TestActor.resetJoinCompletion(actorId);
        assertEquals(
                "scheduled",
                host.actorClient()
                        .requestToActor(actorId, new JoinSpot(SPOT_ID))
                        .timeout(Duration.ofSeconds(5))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS));
        assertInstanceOf(
                ZLinkActorJoinCompletion.Accepted.class, completion.get(5, TimeUnit.SECONDS));
        return created.actor();
    }

    private static ZLinkSessionContext awaitSession() throws Exception {
        var session = TestSession.connected.poll(5, TimeUnit.SECONDS);
        assertNotNull(session, "stream Session did not connect");
        return session;
    }

    private static ZLinkAggregateRelocationCoordinator.Request targetRequest(
            ZLinkLocationRepository repository,
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkLocationOwnerToken targetOwner,
            ZLinkMeshNodeDescriptor targetDescriptor)
            throws Exception {
        List<ZLinkAggregateRelocationCoordinator.Participant> participants = new ArrayList<>();
        ZLinkAuthoritySnapshot spot = null;
        for (var participant : request.participants()) {
            var snapshot =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository
                                    .read(participant.authorityKey(), NEVER)
                                    .toCompletableFuture()
                                    .get(5, TimeUnit.SECONDS));
            var kind =
                    participant.objectKind() == ZLinkPlacementObjectKind.ACTOR.value()
                            ? ZLinkPlacementObjectKind.ACTOR
                            : ZLinkPlacementObjectKind.USER_SPOT;
            participants.add(
                    new ZLinkAggregateRelocationCoordinator.Participant(
                            participant.authorityKey(),
                            kind,
                            participant.objectGeneration(),
                            participant.sourceAuthorityOwnerGeneration(),
                            snapshot.storeVersion(),
                            ZLinkAuthorityGenerationTransition.NEW_OWNER,
                            snapshot.payload(),
                            new byte[0]));
            if (kind == ZLinkPlacementObjectKind.USER_SPOT) spot = snapshot;
        }
        assertNotNull(spot);
        return new ZLinkAggregateRelocationCoordinator.Request(
                request.fence().aggregateId(),
                request.fence().aggregateGeneration(),
                1,
                participants,
                request.relocationPayload(),
                new ZLinkMeshNodeDescriptorKey(MESH, TARGET_RID),
                targetDescriptor.lifecycleGeneration(),
                new ZLinkPlacementCapacityBundle(
                        2,
                        1,
                        Optional.of(
                                new ZLinkSpotTypeCapacityDelta(
                                        ZLinkPlacementObjectKind.USER_SPOT, SPOT_TYPE, 1))),
                targetOwner,
                spot.storeVersion());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationRepository repository, RoutingId rid) throws Exception {
        return repository
                .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                .toCompletableFuture()
                .get(5, TimeUnit.SECONDS)
                .items()
                .stream()
                .filter(value -> value.rid().equals(rid))
                .findFirst()
                .orElseThrow();
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid,
            long lifecycleGeneration,
            ZLinkLocationOwnerToken owner,
            String endpoint) {
        return new ZLinkMeshNodeDescriptor(
                MESH,
                rid,
                lifecycleGeneration,
                1,
                endpoint,
                Map.of(MESH, 100),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                SPOT_TYPE,
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0),
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                ACTOR_TYPE,
                                ZLinkObjectMaintenancePolicyKind.RECREATE,
                                false,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("node-entry-00000000-0000-4000-8000-000000000001"),
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
                ZLinkFrameworkRelocationMode.ROLLING_UPDATE, 0, Optional.empty(), 1);
    }

    private static ZLinkInternalMeshNode peer(
            ZLinkServiceM6BWireCodec wire,
            List<ZLinkServiceM6BWireCodec.SessionRelocationSealed> seals,
            List<SubmittedRoute> routes) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("requestSessionRelocationSeal")) {
                                var seal = wire.decodeSessionRelocationSeal((byte[]) arguments[1]);
                                var reply =
                                        ZLinkServiceM6BWireCodec.SessionRelocationSealed.replyTo(
                                                seal,
                                                ServiceWirePilotCodec.SessionSealResult.SEALED);
                                seals.add(reply);
                                return CompletableFuture.completedFuture(
                                        wire.encodeSessionRelocationSealed(reply));
                            }
                            if (method.getName().equals("sendSessionRelocationRoute")) {
                                routes.add(
                                        new SubmittedRoute(
                                                (RoutingId) arguments[0],
                                                wire.decodeSessionRelocationRoute(
                                                        (byte[]) arguments[1])));
                                return CompletableFuture.completedFuture(null);
                            }
                            if (method.getName().equals("setRelocationControlHandler")) {
                                return null;
                            }
                            if (method.getDeclaringClass() == Object.class) {
                                return switch (method.getName()) {
                                    case "toString" -> "session-route-test-peer";
                                    case "hashCode" -> System.identityHashCode(proxy);
                                    case "equals" -> proxy == arguments[0];
                                    default ->
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                };
                            }
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static ZLinkRelocationTransitionClient noOpTransitionClient() {
        return new ZLinkRelocationTransitionClient() {
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
                    RoutingId targetNodeRid,
                    ZLinkSpotRetireControl.Fence fence,
                    Instant preserveAt) {
                return CompletableFuture.completedFuture(Settlement.TARGET_COMMITTED);
            }

            @Override
            public CompletionStage<Void> abort(
                    RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }
        };
    }

    private static final class TestStagingBackend implements StagingBackend {
        @Override
        public CompletionStage<Object> prepareSpot(
                ZLinkUserSpotAggregateStagingOwner.Request request) {
            return CompletableFuture.completedFuture("prepared-spot");
        }

        @Override
        public CompletionStage<Void> restoreSpot(
                Object preparedSpot,
                ZLinkUserSpotAggregateStagingOwner.Request request,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Object> prepareActor(
                ZLinkUserSpotAggregateStagingOwner.ActorParticipant participant,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(participant.actorId());
        }

        @Override
        public CompletionStage<
                        systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn>
                prepareIngress(Object preparedActor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> enqueueIngress(
                Object preparedSpot,
                List<systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn>
                        dispatches,
                String laneId,
                byte[] record,
                java.util.function.Supplier<CompletionStage<Void>> turn,
                CompletableFuture<Void> admission) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publishSpot(Object preparedSpot) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void publishActor(
                Object preparedSpot,
                Object preparedActor,
                String targetSpotId,
                Long targetOwnerGeneration) {}

        @Override
        public void completeActor(Object preparedActor) {}

        @Override
        public void publishTimers(Object preparedSpot) {}

        @Override
        public CompletionStage<Void> discardActor(Object preparedActor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> discardSpot(Object preparedSpot) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private record SubmittedRoute(
            RoutingId destination, ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {}

    private record JoinSpot(String spotId) {}

    public static final class TestActor implements ZLinkActor {
        private static final Map<String, TestActor> instances = new ConcurrentHashMap<>();
        private static final Map<String, CompletableFuture<ZLinkActorJoinCompletion>>
                joinCompletions = new ConcurrentHashMap<>();
        private final ZLinkActorContext context;

        public TestActor(ZLinkActorContext context) {
            this.context = context;
            instances.put(context.actorId(), this);
            joinCompletions.put(context.actorId(), new CompletableFuture<>());
        }

        static CompletableFuture<ZLinkActorJoinCompletion> resetJoinCompletion(String actorId) {
            var completion = new CompletableFuture<ZLinkActorJoinCompletion>();
            joinCompletions.put(actorId, completion);
            return completion;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joinCompletions.get(context.actorId()).complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor(context));
        }
    }

    public static final class TestEntrySpot implements ZLinkEntrySpot<TestActor> {
        private final ZLinkEntrySpotContext context;

        public TestEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<
                    TestEntrySpot, TestActor, JoinSpot, String> {
        @Override
        public CompletionStage<String> handle(
                TestEntrySpot spot,
                TestActor actor,
                ZLinkMessageContext context,
                JoinSpot request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public TestSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
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

    public static final class TestSpotAdapter implements ZLinkSpotRelocationAdapter<TestSpot> {
        @Override
        public CompletionStage<byte[]> capture(
                TestSpot spot,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[] {1});
        }

        @Override
        public CompletionStage<Void> restore(
                TestSpot spot,
                byte[] state,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestSession implements ZLinkSession {
        private static final LinkedBlockingQueue<ZLinkSessionContext> connected =
                new LinkedBlockingQueue<>();
        private final ZLinkSessionContext context;

        public TestSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            connected.add(context);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
