package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Source-local admission of an Actor send ends when the owning Actor queue accepts the message
 * (spec server/01-execution/01-submit-and-completion §2, §4). A target {@code OnJoinedActor} that
 * awaits a send to the joining Actor must therefore not wait for the Join it is part of, and the
 * payload still runs only after the Join completion callback (spec 03-spot-actor/05 §4).
 */
final class ZLinkJoinedActorSendAdmissionTest {
    private static final long WAIT_SECONDS = 10;
    private static final String PLAYER = "player-1";
    private static final List<String> EVENTS = new CopyOnWriteArrayList<>();
    private static final AtomicReference<ZLinkActorClient> TABLE_CLIENT = new AtomicReference<>();
    private static CompletableFuture<ZLinkActorJoinCompletion> joinCompleted;
    private static CompletableFuture<Void> pingHandled;

    @BeforeEach
    void reset() {
        EVENTS.clear();
        TABLE_CLIENT.set(null);
        joinCompleted = new CompletableFuture<>();
        pingHandled = new CompletableFuture<>();
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void joinedCallbackSendToJoiningActorIsAdmittedBeforeTheJoinCompletes(boolean crossNode)
            throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source =
                start(options("send-join-source", locations, relocations, null, !crossNode))) {
            ZLinkFrameworkRuntime target =
                    crossNode
                            ? start(
                                    options(
                                            "send-join-target",
                                            locations,
                                            relocations,
                                            source.listenerStatus(
                                                            ZLinkListenerKind.ROUTE_MESH, "game")
                                                    .endpoint(),
                                            true))
                            : source;
            try {
                TABLE_CLIENT.set(target.actorClient());
                target.spotManager()
                        .getOrCreate("table-1", "table")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                source.actorManager()
                        .create(PLAYER, "player")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);

                assertEquals(
                        "scheduled",
                        source.actorClient()
                                .requestToActor(PLAYER, new JoinRequest("table-1"))
                                .timeout(Duration.ofSeconds(WAIT_SECONDS))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS));

                var completion = joinCompleted.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        completion,
                        () -> "Join completion " + describe(completion) + " events " + EVENTS);
                pingHandled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(List.of("joined", "send admitted", "completed", "ping"), EVENTS);
            } finally {
                if (crossNode) target.close();
            }
        }
    }

    @Test
    void sendWithoutHandlerFailsAdmissionAsNotFound() throws Exception {
        try (var runtime = start(options("send-missing-handler", null, null, null, true))) {
            runtime.actorManager()
                    .create(PLAYER, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    runtime.actorClient()
                                            .sendToActor(PLAYER, new Unhandled())
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    @Test
    void requestWithoutHandlerEndsWithoutHanging() throws Exception {
        try (var runtime = start(options("request-missing-handler", null, null, null, true))) {
            runtime.actorManager()
                    .create(PLAYER, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    runtime.actorClient()
                                            .requestToActor(PLAYER, new Unhandled())
                                            .timeout(Duration.ofSeconds(WAIT_SECONDS * 3))
                                            .submit(String.class)
                                            .toCompletableFuture()
                                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
        }
    }

    private static String describe(ZLinkActorJoinCompletion completion) {
        return completion instanceof ZLinkActorJoinCompletion.Failed failed
                ? "Failed(" + failed.kind() + ")"
                : String.valueOf(completion);
    }

    private static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String sourceEndpoint,
            boolean hostsTable) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations == null ? new ZLinkInMemoryLocationStore() : locations);
        options.addRelocationStore(
                relocations == null ? new InMemoryRelocationStore() : relocations);
        var node = options.addRouteMesh("game");
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(rid));
        if (sourceEndpoint != null) node.peerConnections().connect(sourceEndpoint);
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.preserveStateWith(EmptyState.class));
        if (hostsTable) {
            objects.addSpotFactory("table", Table.class, factory -> factory.disableRelocation());
        }
        return options;
    }

    public record JoinRequest(String spotId) {}

    public record Ping() {}

    public record Unhandled() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            EVENTS.add("completed");
            joinCompleted.complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class EmptyState implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
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
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Table implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public Table(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(PingHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        /** The board-tables shape: the joined callback awaits a send to the joining Actor. */
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            EVENTS.add("joined");
            return TABLE_CLIENT
                    .get()
                    .sendToActor(actor.context().actorId(), new Ping())
                    .submit()
                    .thenRun(() -> EVENTS.add("send admitted"));
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class PingHandler
            implements ZLinkSpotActorSendHandler<Table, Player, Ping> {
        @Override
        public CompletionStage<Void> handle(
                Table spot, Player actor, ZLinkMessageContext context, Ping message) {
            EVENTS.add("ping");
            pingHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
