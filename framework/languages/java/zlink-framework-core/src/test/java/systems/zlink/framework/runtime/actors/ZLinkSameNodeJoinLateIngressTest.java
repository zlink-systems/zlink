package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

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
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkSameNodeJoinLateIngressTest {
    private static final String ROOM = "room";
    private static final String ACTOR = "player";
    private static final Duration REQUEST_DEADLINE = Duration.ofSeconds(2);
    private static final CompletableFuture<Void> DONE = CompletableFuture.completedFuture(null);

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void lateRequestRequiresSuccessfulJoinCompletion(boolean failCompletion) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh("game");
        node.listen("inproc://same-node-join-late-" + System.nanoTime())
                .setRoutingId(RoutingId.from("same-node-join-late"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());

        try (var drops = new NoticeDrops();
                var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            await(runtime.spotManager().getOrCreate(ROOM, "room").submit());
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    await(runtime.actorManager().create(ACTOR, "player").submit()));

            var actors = ZLinkFrameworkRuntimeTestAccess.actors(runtime);
            var player = assertInstanceOf(Player.class, actors.actorById(ACTOR));
            var sourceLane = await(actors.actorRelocationLaneAsync(ACTOR));
            var client = runtime.actorClient();
            assertEquals(
                    "scheduled",
                    await(
                            client.requestToActor(ACTOR, new Join(failCompletion))
                                    .timeout(REQUEST_DEADLINE)
                                    .submit(String.class)));
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(player.completion));
            // Post a lifecycle turn behind the source Join barrier to observe its settlement.
            await(sourceLane.enqueueLifecycleBarrier(() -> DONE));
            assertEquals(ROOM, player.context().spotId().orElseThrow());

            long started = System.nanoTime();
            String reply = null;
            Throwable failure = null;
            try {
                reply =
                        await(
                                client.requestToActor(ACTOR, new Probe("late"))
                                        .timeout(REQUEST_DEADLINE)
                                        .submit(String.class));
            } catch (ExecutionException error) {
                failure = error.getCause();
            }
            Duration elapsed = Duration.ofNanos(System.nanoTime() - started);
            if (failCompletion) {
                assertNotNull(
                        failure,
                        () ->
                                "Late request replied after "
                                        + elapsed
                                        + ", handler calls="
                                        + player.requests.get());
                var rejected = assertInstanceOf(ZLinkFrameworkException.class, failure);
                assertAll(
                        () ->
                                assertEquals(
                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        rejected.kind(),
                                        failureMessage(elapsed, player)),
                        () ->
                                assertTrue(
                                        elapsed.compareTo(Duration.ofSeconds(1)) < 0,
                                        failureMessage(elapsed, player)),
                        () -> assertEquals(0, player.requests.get()));
                await(client.sendToActor(ACTOR, new Notice()).submit());
                assertTrue(await(drops.notice).contains("reason=activation_rejected"));
                assertEquals(0, player.notices.get());
            } else {
                assertEquals("late", reply);
                assertEquals(1, player.requests.get());
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void remoteLateIngressRequiresSuccessfulJoinCompletion(boolean failCompletion)
            throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var drops = new NoticeDrops();
                var source =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                remoteOptions(
                                        "join-source", locations, relocations, null, false))) {
            ready(source);
            var original =
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    await(source.actorManager().create(ACTOR, "player").submit()))
                            .actor();
            assertEquals(RoutingId.from("join-source"), original.nodeRid());

            String sourceEndpoint =
                    source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint();
            var targetOptions =
                    remoteOptions("join-target", locations, relocations, sourceEndpoint, true);
            try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions)) {
                ready(target);
                awaitPeer(target);
                String targetEndpoint =
                        target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint();
                var senderOptions =
                        remoteOptions("join-sender", locations, relocations, targetEndpoint, false);
                try (var sender = ZLinkFrameworkRuntimeTestAccess.start(senderOptions)) {
                    ready(sender);
                    awaitPeer(sender);
                    var room =
                            await(target.spotManager().getOrCreate(ROOM, "room").submit()).spot();
                    assertEquals(
                            "scheduled",
                            await(
                                    source.actorClient()
                                            .requestToActor(ACTOR, new Join(failCompletion))
                                            .timeout(REQUEST_DEADLINE)
                                            .submit(String.class)));

                    var actors = ZLinkFrameworkRuntimeTestAccess.actors(target);
                    var player = assertInstanceOf(Player.class, awaitTargetActor(actors));
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Accepted.class, await(player.completion));
                    var moved = await(sender.actorManager().find(ACTOR)).orElseThrow();
                    assertEquals(room.nodeRid(), moved.nodeRid());
                    assertEquals(original.objectGeneration(), moved.objectGeneration());

                    verifyRemoteIngress(sender, player, drops, failCompletion);
                }
            }
        }
    }

    private static void verifyRemoteIngress(
            ZLinkFrameworkRuntime sender, Player player, NoticeDrops drops, boolean failCompletion)
            throws Exception {
        var client = sender.actorClient();
        long started = System.nanoTime();
        var result =
                await(
                        client.requestToActor(ACTOR, new Probe("late"))
                                .timeout(REQUEST_DEADLINE)
                                .submit(String.class)
                                .handle(
                                        (reply, failure) ->
                                                new ProbeResult(
                                                        reply,
                                                        failure,
                                                        Duration.ofNanos(
                                                                System.nanoTime() - started))));
        await(client.sendToActor(ACTOR, new Notice()).submit());
        boolean noticeSettled;
        try {
            await(CompletableFuture.anyOf(drops.notice, player.noticeHandled));
            noticeSettled = true;
        } catch (TimeoutException noDelivery) {
            noticeSettled = false;
        }

        if (failCompletion) {
            String outcome = result + ", " + failureMessage(result.elapsed(), player);
            assertAll(
                    () -> {
                        var error =
                                assertInstanceOf(
                                        ZLinkFrameworkException.class, result.failure(), outcome);
                        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind(), outcome);
                    },
                    () ->
                            assertTrue(
                                    result.elapsed().compareTo(Duration.ofSeconds(1)) < 0, outcome),
                    () -> assertEquals(0, player.requests.get()),
                    () -> assertEquals(0, player.notices.get()),
                    () ->
                            assertTrue(
                                    drops.notice.getNow("").contains("reason=activation_rejected"),
                                    "receiver did not report the send drop"));
        } else {
            assertTrue(noticeSettled, "notice was not delivered");
            assertAll(
                    () -> assertNull(result.failure()),
                    () -> assertEquals("late", result.reply()),
                    () -> assertEquals(1, player.requests.get()),
                    () -> assertEquals(1, player.notices.get()));
        }
    }

    private record ProbeResult(String reply, Throwable failure, Duration elapsed) {}

    private static DefaultZLinkFrameworkOptions remoteOptions(
            String routingId,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer,
            boolean hostsRoom) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node =
                options.addRouteMesh("game")
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(routingId));
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "player",
                Player.class,
                Factory.class,
                factory -> factory.preserveStateWith(State.class));
        if (hostsRoom)
            objects.addSpotFactory("room", Room.class, factory -> factory.recreateOnRelocation());
        return options;
    }

    private static void ready(ZLinkFrameworkRuntime runtime) throws Exception {
        await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
        assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
    }

    private static ZLinkActor awaitTargetActor(ZLinkActorRuntime actors)
            throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(4).toNanos();
        ZLinkActor actor;
        while ((actor = actors.actorById(ACTOR)) == null && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertNotNull(actor, "target Actor was not materialized");
        return actor;
    }

    private static void awaitPeer(ZLinkFrameworkRuntime runtime) throws Exception {
        var ready = new CompletableFuture<Void>();
        var subscription = new CompletableFuture<Flow.Subscription>();
        runtime.routeMeshRuntime()
                .observe("game", 8)
                .subscribe(
                        new Flow.Subscriber<>() {
                            @Override
                            public void onSubscribe(Flow.Subscription value) {
                                subscription.complete(value);
                                value.request(Long.MAX_VALUE);
                            }

                            @Override
                            public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> value) {
                                if (value.status().isReady() && value.status().readyPeerCount() > 0)
                                    ready.complete(null);
                            }

                            @Override
                            public void onError(Throwable failure) {
                                ready.completeExceptionally(failure);
                            }

                            @Override
                            public void onComplete() {}
                        });
        try {
            await(ready);
        } finally {
            await(subscription).cancel();
        }
    }

    private static String failureMessage(Duration elapsed, Player player) {
        return "elapsed=" + elapsed + ", handler calls=" + player.requests.get();
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(4, TimeUnit.SECONDS);
    }

    private static final class NoticeDrops extends Handler implements AutoCloseable {
        private final Logger logger =
                Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        private final CompletableFuture<String> notice = new CompletableFuture<>();

        NoticeDrops() {
            logger.addHandler(this);
        }

        @Override
        public void publish(LogRecord record) {
            String message = record.getMessage();
            if (message != null
                    && message.contains("event_id=zlink.dispatch_error")
                    && message.contains("packet=Notice ")
                    && message.contains("action=drop")) {
                notice.complete(message);
            }
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            logger.removeHandler(this);
        }
    }

    public record Join(boolean failCompletion) {}

    public record Probe(String value) {}

    public record Notice() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        private final CompletableFuture<ZLinkActorJoinCompletion> completion =
                new CompletableFuture<>();
        private final AtomicInteger requests = new AtomicInteger();
        private final AtomicInteger notices = new AtomicInteger();
        private final CompletableFuture<Void> noticeHandled = new CompletableFuture<>();
        private boolean failCompletion;

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            completion.complete(result);
            return failCompletion
                    ? CompletableFuture.failedFuture(
                            new IllegalStateException("Join completion failed"))
                    : DONE;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class State implements ZLinkActorRelocationAdapter<Player> {
        @Override
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(
                    new byte[] {(byte) (actor.failCompletion ? 1 : 0)});
        }

        @Override
        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            actor.failCompletion = state[0] != 0;
            return DONE;
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return DONE;
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return DONE;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return DONE;
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return DONE;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
            context.handlers().addHandler(NoticeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.failCompletion = request.failCompletion();
            actor.context().joinSpot(ROOM).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Probe, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Probe request) {
            actor.requests.incrementAndGet();
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class NoticeHandler
            implements ZLinkSpotActorSendHandler<Room, Player, Notice> {
        @Override
        public CompletionStage<Void> handle(
                Room spot, Player actor, ZLinkMessageContext context, Notice notice) {
            actor.notices.incrementAndGet();
            actor.noticeHandled.complete(null);
            return DONE;
        }
    }
}
