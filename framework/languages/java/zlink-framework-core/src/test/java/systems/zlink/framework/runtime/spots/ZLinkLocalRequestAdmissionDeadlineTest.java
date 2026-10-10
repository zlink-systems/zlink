package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkLocalRequestAdmissionDeadlineTest {
    private static final AtomicInteger handled = new AtomicInteger();

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void deadlineEndsLocalAdmissionAndRemovesCapacityWaiter(boolean actor) throws Exception {
        verifyDeadline(actor, false);
    }

    @Test
    void directLocalSpotDeadlineEndsAdmissionAndRemovesCapacityWaiter() throws Exception {
        verifyDeadline(false, true);
    }

    private void verifyDeadline(boolean actor, boolean direct) throws Exception {
        handled.set(0);
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.registration().inboundDispatch().setMaxQueuedApplicationJobs(1);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("deadline")
                        .listen("inproc://local-deadline-" + System.nanoTime())
                        .setRoutingId(RoutingId.from("local-deadline"))
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, f -> f.disableRelocation());
        objects.addActorFactory("actor", Actor.class, Factory.class, f -> f.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                    .getOrCreate("room-a", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            runtime.actorManager()
                    .create("actor-a", "actor")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var hostField = runtime.getClass().getDeclaredField("spots");
            hostField.setAccessible(true);
            var host = hostField.get(runtime);
            var queueField = host.getClass().getDeclaredField("applicationJobQueue");
            queueField.setAccessible(true);
            var queue = (ZLinkApplicationJobQueue) queueField.get(host);
            var nodeField = host.getClass().getDeclaredField("primaryNode");
            nodeField.setAccessible(true);
            var node =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode)
                            nodeField.get(host);
            var target =
                    ((ZLinkSpotRuntime) host)
                            .spotLifecycle()
                            .spotActivationFor("room-a")
                            .backendSpot;
            var executor = Executors.newSingleThreadExecutor();
            try (var held =
                    queue.acquire(
                                    systems.zlink.framework.runtime.internal.dispatch
                                            .ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)) {
                var submitted =
                        executor.submit(
                                () ->
                                        direct
                                                ? directRequest(node.entrySpot(), target)
                                                : actor
                                                        ? runtime.actorClient()
                                                                .requestToActor(
                                                                        "actor-a", new Probe())
                                                                .timeout(Duration.ofMillis(100))
                                                                .submit(String.class)
                                                                .toCompletableFuture()
                                                        : runtime.route()
                                                                .requestToSpot(
                                                                        "room-a", new Probe())
                                                                .timeout(Duration.ofMillis(100))
                                                                .submit(String.class)
                                                                .toCompletableFuture());
                var result = submitted.get(2, TimeUnit.SECONDS);
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> result.get(2, TimeUnit.SECONDS));
                if (direct) {
                    assertEquals(
                            systems.zlink.contracts.sockets.RequestResult.TIMED_OUT,
                            assertInstanceOf(
                                            systems.zlink.contracts.errors.ZlinkRequestException
                                                    .class,
                                            failure.getCause())
                                    .getResult());
                } else {
                    assertEquals(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind(),
                            () ->
                                    failure.getCause().toString()
                                            + " cause="
                                            + failure.getCause().getCause());
                }
                assertEquals(0, handled.get());
                assertEquals(1, queue.snapshot().permitsInUse());
                assertEquals(0, queue.snapshot().capacityWaiters());
            } finally {
                executor.shutdownNow();
                assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS));
            }
            var followup =
                    actor
                            ? runtime.actorClient()
                                    .requestToActor("actor-a", new Probe())
                                    .timeout(Duration.ofSeconds(1))
                                    .submit(String.class)
                            : runtime.route()
                                    .requestToSpot("room-a", new Probe())
                                    .timeout(Duration.ofSeconds(1))
                                    .submit(String.class);
            assertEquals("reply", followup.toCompletableFuture().get(2, TimeUnit.SECONDS));
            assertEquals(1, handled.get());
            assertEquals(0, queue.snapshot().permitsInUse());
            assertEquals(0, queue.snapshot().capacityWaiters());
        }
    }

    private CompletableFuture<String> directRequest(
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot source,
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot target) {
        try (var part = systems.zlink.contracts.messaging.Message.from("probe")) {
            return source.requestToSpot(
                            RoutingId.from("local-deadline"),
                            target.spotId(),
                            target.lifecycleGeneration(),
                            java.util.List.of(part),
                            Duration.ofMillis(100))
                    .thenApply(
                            reply -> {
                                try (reply) {
                                    return reply.parts().getFirst().toUtf8String();
                                }
                            })
                    .toCompletableFuture();
        }
    }

    public record Probe() {}

    public static final class Actor implements ZLinkActor {
        private final ZLinkActorContext context;

        public Actor(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Actor(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Actor> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(ActorHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Room implements ZLinkSpot<Actor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(SpotHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ActorHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Actor, Probe, String> {
        public CompletionStage<String> handle(
                Entry spot, Actor actor, ZLinkMessageContext context, Probe request) {
            handled.incrementAndGet();
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class SpotHandler implements ZLinkSpotRequestHandler<Room, Probe, String> {
        public CompletionStage<String> handle(Room spot, Probe request) {
            handled.incrementAndGet();
            return CompletableFuture.completedFuture("reply");
        }
    }
}
