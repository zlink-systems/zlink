package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class SpotActorTransferContractTest {
    private static final AtomicReference<ContractActor> ACTOR = new AtomicReference<>();
    private static CountDownLatch joinCompleted;
    private static CountDownLatch targetJoined;
    private static final AtomicReference<ZLinkActorJoinCompletion> JOIN_RESULT =
            new AtomicReference<>();
    private static boolean includeJoinReply;

    @BeforeEach
    void reset() {
        ACTOR.set(null);
        JOIN_RESULT.set(null);
        includeJoinReply = false;
        joinCompleted = new CountDownLatch(1);
        targetJoined = new CountDownLatch(1);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void crossNodeDeferredJoinPreservesEmptyApplicationState(boolean withReply) throws Exception {
        includeJoinReply = withReply;
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceOptions = relocationOptions("join-source", locations, relocations, null);
        try (var source =
                RuntimeTestSupport.startFramework(
                        sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            source.actorManager()
                                    .create("empty-state-player", "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(3, TimeUnit.SECONDS));
            var targetOptions =
                    relocationOptions(
                            "join-target",
                            locations,
                            relocations,
                            source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint());
            try (var target =
                    RuntimeTestSupport.startFramework(
                            targetOptions, new ZLinkJavaBackendAdapterFactory())) {
                var room =
                        target.spotManager()
                                .getOrCreate("target-room", "target")
                                .submit()
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS);
                assertEquals(RoutingId.from("join-source"), created.actor().nodeRid());
                assertEquals(RoutingId.from("join-target"), room.spot().nodeRid());
                assertEquals(
                        "scheduled",
                        source.actorClient()
                                .requestToActor(
                                        created.actor().actorId(), new JoinRequest("target-room"))
                                .timeout(Duration.ofSeconds(3))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
                assertTrue(joinCompleted.await(3, TimeUnit.SECONDS));
                var accepted =
                        assertInstanceOf(
                                ZLinkActorJoinCompletion.Accepted.class, JOIN_RESULT.get());
                assertEquals(RoutingId.from("join-target"), accepted.actor().nodeRid());
                if (withReply) {
                    assertEquals(
                            new JoinReply("welcome"), accepted.reply().decode(JoinReply.class));
                } else {
                    assertTrue(accepted.reply().isEmpty());
                }
                assertTrue(targetJoined.await(3, TimeUnit.SECONDS));
                assertEquals(
                        "target-room",
                        target.actorClient()
                                .requestToActor(created.actor().actorId(), new ProbeRequest())
                                .timeout(Duration.ofSeconds(3))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
            }
        }
    }

    private static DefaultZLinkFrameworkOptions relocationOptions(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String sourceEndpoint) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        var node = options.addRouteMesh("game");
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(rid));
        node.objects()
                .server()
                .addEntrySpot(ContractEntrySpot.class)
                .addActorFactory(
                        "player",
                        ContractActor.class,
                        ContractActorFactory.class,
                        factory -> factory.preserveStateWith(EmptyStateAdapter.class));
        if (sourceEndpoint != null) {
            node.peerConnections().connect(sourceEndpoint);
            node.objects()
                    .server()
                    .addSpotFactory(
                            "target",
                            ContractTargetSpot.class,
                            factory -> factory.disableRelocation());
        }
        return options;
    }

    public static final class EmptyStateAdapter
            implements ZLinkActorRelocationAdapter<ContractActor> {
        @Override
        public CompletionStage<byte[]> capture(
                ContractActor actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                ContractActor actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            assertEquals(0, state.length);
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void deferredJoinUsesCurrentSpotIdContractAndCompletesOnActorTurn() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("inproc://deferred-join-" + System.nanoTime())
                .setRoutingId(RoutingId.from("deferred-join-node"));
        var objects = node.objects().server();
        objects.addEntrySpot(ContractEntrySpot.class);
        objects.addSpotFactory(
                "target", ContractTargetSpot.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player",
                ContractActor.class,
                ContractActorFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                    .getOrCreate("target-room", "target")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            ZLinkActorCreateResult.Created created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            runtime.actorManager()
                                    .create("player-1", "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(3, TimeUnit.SECONDS));

            String scheduled =
                    runtime.actorClient()
                            .requestToActor(
                                    created.actor().actorId(), new JoinRequest("target-room"))
                            .timeout(Duration.ofSeconds(3))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);

            assertEquals("scheduled", scheduled);
            assertTrue(targetJoined.await(3, TimeUnit.SECONDS));
            assertTrue(joinCompleted.await(3, TimeUnit.SECONDS));
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, JOIN_RESULT.get());
            assertEquals("target-room", ACTOR.get().context().spotId().orElseThrow());

            String currentSpot =
                    runtime.actorClient()
                            .requestToActor(created.actor().actorId(), new ProbeRequest())
                            .timeout(Duration.ofSeconds(3))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
            assertEquals("target-room", currentSpot);
        }
    }

    public static final class ContractActor implements ZLinkActor {
        private final ZLinkActorContext context;

        public ContractActor(ZLinkActorContext context) {
            this.context = context;
            ACTOR.set(this);
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            JOIN_RESULT.set(completion);
            joinCompleted.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ContractActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ContractActor(context));
        }
    }

    public static final class ContractEntrySpot implements ZLinkEntrySpot<ContractActor> {
        private final ZLinkEntrySpotContext context;

        public ContractEntrySpot(ZLinkEntrySpotContext context) {
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
        public CompletionStage<Void> onJoinedActor(ContractActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ContractActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ContractTargetSpot implements ZLinkSpot<ContractActor> {
        private final ZLinkSpotContext context;

        public ContractTargetSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                    includeJoinReply
                            ? ZLinkSpotActorJoinResult.accept(new JoinReply("welcome"))
                            : ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ContractActor actor) {
            targetJoined.countDown();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ContractActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record JoinRequest(String spotId) {}

    public record JoinReply(String value) {}

    public record ProbeRequest() {}

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<
                    ContractEntrySpot, ContractActor, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                ContractEntrySpot spot,
                ContractActor actor,
                ZLinkMessageContext context,
                JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<
                    ContractTargetSpot, ContractActor, ProbeRequest, String> {
        @Override
        public CompletionStage<String> handle(
                ContractTargetSpot spot,
                ContractActor actor,
                ZLinkMessageContext context,
                ProbeRequest request) {
            return CompletableFuture.completedFuture(actor.context().spotId().orElseThrow());
        }
    }
}
