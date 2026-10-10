package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkLocalAdmissionOrderTest {
    private static final int MESSAGE_COUNT = 16;
    private static final java.util.List<Integer> received =
            new java.util.concurrent.CopyOnWriteArrayList<>();
    private static CompletableFuture<Void> allHandled;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localMessagesKeepSubmissionOrderAfterCapacityWait(boolean actor) throws Exception {
        received.clear();
        var logger =
                java.util.logging.Logger.getLogger(
                        systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                .getName());
        var previousLevel = logger.getLevel();
        var log = new java.util.logging.FileHandler("build/local-admission-" + actor + ".flow");
        logger.addHandler(log);
        logger.setLevel(java.util.logging.Level.ALL);
        allHandled = new CompletableFuture<>();
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.registration().inboundDispatch().setMaxQueuedApplicationJobs(1);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("order")
                        .listen("inproc://local-order-" + System.nanoTime())
                        .setRoutingId(RoutingId.from("local-order"))
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
            var submissions = new java.util.ArrayList<CompletableFuture<?>>();
            try (var held =
                    queue.acquire(
                                    systems.zlink.framework.runtime.internal.dispatch
                                            .ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)) {
                for (int index = 0; index < MESSAGE_COUNT; index++) {
                    var probe = new Probe(index);
                    submissions.add(
                            actor
                                    ? runtime.actorClient()
                                            .sendToActor("actor-a", probe)
                                            .submit()
                                            .toCompletableFuture()
                                    : runtime.route()
                                            .requestToSpot("room-a", probe)
                                            .submit(String.class)
                                            .toCompletableFuture());
                }
                assertTrue(received.isEmpty());
                assertEquals(MESSAGE_COUNT, queue.snapshot().capacityWaiters());
            }
            try {
                allHandled.get(5, TimeUnit.SECONDS);
            } catch (java.util.concurrent.TimeoutException failure) {
                fail(
                        "received="
                                + received
                                + " queue="
                                + queue.snapshot()
                                + " submissions="
                                + submissions,
                        failure);
            }
            CompletableFuture.allOf(submissions.toArray(CompletableFuture[]::new))
                    .get(5, TimeUnit.SECONDS);
            assertEquals(
                    java.util.stream.IntStream.range(0, MESSAGE_COUNT).boxed().toList(), received);
            assertEquals(0, queue.snapshot().capacityWaiters());
            assertEquals(0, queue.snapshot().permitsInUse());
        } finally {
            logger.removeHandler(log);
            logger.setLevel(previousLevel);
            log.close();
        }
    }

    public record Probe(int index) {}

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
            implements systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler<
                    Entry, Actor, Probe> {
        public CompletionStage<Void> handle(
                Entry spot, Actor actor, ZLinkMessageContext context, Probe request) {
            received.add(request.index());
            if (received.size() == MESSAGE_COUNT) allHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class SpotHandler implements ZLinkSpotRequestHandler<Room, Probe, String> {
        public CompletionStage<String> handle(Room spot, Probe request) {
            received.add(request.index());
            if (received.size() == MESSAGE_COUNT) allHandled.complete(null);
            return CompletableFuture.completedFuture("reply");
        }
    }
}
