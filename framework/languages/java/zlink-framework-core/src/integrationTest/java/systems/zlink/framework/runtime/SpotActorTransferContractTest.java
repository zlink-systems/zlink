package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.messaging.ZLinkMessage;
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

final class SpotActorTransferContractTest {
    private static final AtomicReference<ContractActor> ACTOR = new AtomicReference<>();
    private static CountDownLatch joinCompleted;
    private static CountDownLatch targetJoined;

    @BeforeEach
    void reset() {
        ACTOR.set(null);
        joinCompleted = new CountDownLatch(1);
        targetJoined = new CountDownLatch(1);
    }

    @Test
    void deferredJoinUsesCurrentSpotIdContractAndCompletesOnActorTurn()
        throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("inproc://deferred-join-" + System.nanoTime())
            .setRoutingId(RoutingId.from("deferred-join-node"));
        var objects = node.objects().server();
        objects.addEntrySpot(ContractEntrySpot.class);
        objects.addSpotFactory(
            "target",
            ContractTargetSpot.class,
            factory -> factory.disableRelocation());
        objects.addActorFactory(
            "player",
            ContractActor.class,
            ContractActorFactory.class,
            factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime = RuntimeTestSupport.startFramework(
            options,
            new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                .getOrCreate("target-room", "target")
                .submit()
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
            ZLinkActorCreateResult.Created created = assertInstanceOf(
                ZLinkActorCreateResult.Created.class,
                runtime.actorManager()
                    .create("player-1", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS));

            String scheduled = runtime.actorClient()
                .requestToActor(created.actor().actorId(), new JoinRequest("target-room"))
                .timeout(Duration.ofSeconds(3))
                .submit(String.class)
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS);

            assertEquals("scheduled", scheduled);
            assertTrue(targetJoined.await(3, TimeUnit.SECONDS));
            assertTrue(joinCompleted.await(3, TimeUnit.SECONDS));
            assertEquals("target-room", ACTOR.get().context().spotId().orElseThrow());

            String currentSpot = runtime.actorClient()
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
        public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, completion);
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

    public static final class ContractEntrySpot
        implements ZLinkEntrySpot<ContractActor> {
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
            String actorId,
            ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept());
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

    public record JoinRequest(String spotId) {
    }

    public record ProbeRequest() {
    }

    public static final class JoinHandler implements
        ZLinkEntrySpotActorRequestHandler<
            ContractEntrySpot,
            ContractActor,
            JoinRequest,
            String> {
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

    public static final class ProbeHandler implements ZLinkSpotActorRequestHandler<
        ContractTargetSpot,
        ContractActor,
        ProbeRequest,
        String> {
        @Override
        public CompletionStage<String> handle(
            ContractTargetSpot spot,
            ContractActor actor,
            ZLinkMessageContext context,
            ProbeRequest request) {
            return CompletableFuture.completedFuture(
                actor.context().spotId().orElseThrow());
        }
    }
}
