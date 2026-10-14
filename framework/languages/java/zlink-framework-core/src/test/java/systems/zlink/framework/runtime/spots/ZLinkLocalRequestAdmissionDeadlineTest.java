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
import systems.zlink.framework.channels.ZLinkRequestStage;
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
    private static final int IMMEDIATE_FAILURE_REQUEST_COUNT = 128;
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

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localAdmissionCompletesBeforeTheReply(boolean actor) throws Exception {
        heldReply = new CompletableFuture<>();
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var result = submitLocal(runtime, actor, Duration.ofSeconds(1), true);
            result.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertFalse(result.toCompletableFuture().isDone());
            heldReply.complete("reply");
            assertEquals("reply", result.toCompletableFuture().get(2, TimeUnit.SECONDS));
        } finally {
            heldReply.complete("reply");
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localCapacityWaitSeparatesAdmissionFromReply(boolean actor) throws Exception {
        heldReply = new CompletableFuture<>();
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var queue = applicationQueue(runtime);
            ZLinkRequestStage<String> result;
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS)) {
                result = submitLocal(runtime, actor, Duration.ofSeconds(1), true);
                awaitCapacityWaiter(queue);
                assertFalse(result.admitted().toCompletableFuture().isDone());
                assertFalse(result.toCompletableFuture().isDone());
            }
            result.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertFalse(result.toCompletableFuture().isDone());
            heldReply.complete("reply");
            assertEquals("reply", result.toCompletableFuture().get(2, TimeUnit.SECONDS));
        } finally {
            heldReply.complete("reply");
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localTimeoutBeforeAdmissionFailsBothStagesWithTheSameCause(boolean actor)
            throws Exception {
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var queue = applicationQueue(runtime);
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS)) {
                var result = submitLocal(runtime, actor, Duration.ofMillis(100), false);
                var replyFailure =
                        assertThrows(
                                ExecutionException.class,
                                () -> result.toCompletableFuture().get(2, TimeUnit.SECONDS));
                var admissionFailure =
                        assertThrows(
                                ExecutionException.class,
                                () ->
                                        result.admitted()
                                                .toCompletableFuture()
                                                .get(2, TimeUnit.SECONDS));
                assertSame(replyFailure.getCause(), admissionFailure.getCause());
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, replyFailure.getCause())
                                .kind());
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localCancellationBeforeAdmissionFailsBothStagesWithTheSameCause(boolean actor)
            throws Exception {
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var queue = applicationQueue(runtime);
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS)) {
                var result = submitLocal(runtime, actor, Duration.ofSeconds(1), false);
                awaitCapacityWaiter(queue);
                assertTrue(result.toCompletableFuture().cancel(false));
                var replyFailure =
                        result.handle((ignored, failure) -> failure).toCompletableFuture().join();
                var admissionFailure =
                        result.admitted()
                                .handle((ignored, failure) -> failure)
                                .toCompletableFuture()
                                .join();
                assertInstanceOf(java.util.concurrent.CancellationException.class, replyFailure);
                assertSame(replyFailure, admissionFailure);
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void completedLocalCallsHaveIndependentAdmissionStages(boolean actor) throws Exception {
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var first = submitLocal(runtime, actor, Duration.ofSeconds(1), false);
            assertEquals("reply", first.toCompletableFuture().get(2, TimeUnit.SECONDS));
            var second = submitLocal(runtime, actor, Duration.ofSeconds(1), false);
            assertEquals("reply", second.toCompletableFuture().get(2, TimeUnit.SECONDS));
            first.admitted()
                    .toCompletableFuture()
                    .obtrudeException(new IllegalStateException("caller mutation"));
            second.admitted().toCompletableFuture().join();
            assertEquals("reply", second.toCompletableFuture().join());
            var third = submitLocal(runtime, actor, Duration.ofSeconds(1), false);
            third.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertEquals("reply", third.toCompletableFuture().get(2, TimeUnit.SECONDS));
        }
    }

    @Test
    void ownerRejectionBeforeQueueCommitFailsAdmissionWithTheReplyCause() throws Exception {
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var actor =
                    runtime.actorManager()
                            .find("actor-a")
                            .toCompletableFuture()
                            .get()
                            .orElseThrow();
            var queue = applicationQueue(runtime);
            ZLinkRequestStage<String> result;
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS)) {
                result = submitLocal(runtime, true, Duration.ofSeconds(1), false);
                awaitCapacityWaiter(queue);
                assertTrue(
                        runtime.actorManager()
                                .destroy(actor)
                                .toCompletableFuture()
                                .get(2, TimeUnit.SECONDS));
                assertFalse(result.admitted().toCompletableFuture().isDone());
            }
            var replyFailure =
                    assertThrows(
                            ExecutionException.class,
                            () -> result.toCompletableFuture().get(2, TimeUnit.SECONDS));
            var admissionFailure =
                    assertThrows(
                            ExecutionException.class,
                            () -> result.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS));
            assertSame(replyFailure.getCause(), admissionFailure.getCause());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void handlerFailureAfterQueueCommitDoesNotFailAdmission(boolean actor) throws Exception {
        var failure = new IllegalStateException("handler failed after admission");
        heldReply = CompletableFuture.failedFuture(failure);
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            for (int index = 0; index < IMMEDIATE_FAILURE_REQUEST_COUNT; index++) {
                var result = submitLocal(runtime, actor, Duration.ofSeconds(1), true);
                assertThrows(
                        ExecutionException.class,
                        () -> result.toCompletableFuture().get(2, TimeUnit.SECONDS));
                assertDoesNotThrow(
                        () -> result.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS));
            }
        }
    }

    @Test
    void admissionContinuationOutsideHandlerStartsOutsideInfrastructureTurn() throws Exception {
        heldReply = new CompletableFuture<>();
        try (var runtime = startAdmissionRuntime()) {
            createLocalTargets(runtime);
            var queue = applicationQueue(runtime);
            ZLinkRequestStage<String> result;
            CompletionStage<String> continuation;
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(2, TimeUnit.SECONDS)) {
                result = submitLocal(runtime, true, Duration.ofSeconds(1), true);
                awaitCapacityWaiter(queue);
                continuation =
                        result.admitted()
                                .thenApply(
                                        ignored -> {
                                            assertFalse(
                                                    java.util.Arrays.stream(
                                                                    Thread.currentThread()
                                                                            .getStackTrace())
                                                            .anyMatch(
                                                                    frame ->
                                                                            frame.getMethodName()
                                                                                            .equals(
                                                                                                    "deliverGrant")
                                                                                    || frame.getMethodName()
                                                                                            .equals(
                                                                                                    "publishApplicationJob")),
                                                    "application continuation ran on the infrastructure publication stack");
                                            return runtime.route()
                                                    .requestToSpot("room-a", new Probe(false))
                                                    .submit_sync(String.class);
                                        });
            }
            assertEquals("reply", continuation.toCompletableFuture().get(2, TimeUnit.SECONDS));
            assertFalse(result.toCompletableFuture().isDone());
            heldReply.complete("reply");
            assertEquals("reply", result.toCompletableFuture().get(2, TimeUnit.SECONDS));
        } finally {
            heldReply.complete("reply");
        }
    }

    @Test
    void queuedActorAdmissionDoesNotWaitForTheActiveHandlerTurn() throws Exception {
        heldReply = new CompletableFuture<>();
        try (var runtime = startAdmissionRuntime()) {
            try {
                createLocalTargets(runtime);
                var first = submitLocal(runtime, true, Duration.ofSeconds(1), true);
                first.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS);
                var second = submitLocal(runtime, true, Duration.ofSeconds(1), true);
                second.admitted().toCompletableFuture().get(2, TimeUnit.SECONDS);
                assertFalse(first.toCompletableFuture().isDone());
                assertFalse(second.toCompletableFuture().isDone());
                heldReply.complete("reply");
                assertEquals("reply", first.toCompletableFuture().get(2, TimeUnit.SECONDS));
                assertEquals("reply", second.toCompletableFuture().get(2, TimeUnit.SECONDS));
            } finally {
                heldReply.complete("reply");
            }
        }
    }

    private systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime startAdmissionRuntime() {
        var options = new DefaultZLinkFrameworkOptions();
        options.registration().inboundDispatch().setMaxQueuedApplicationJobs(1);
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("admission")
                        .listen("inproc://admission-" + System.nanoTime())
                        .setRoutingId(RoutingId.from("admission-local"))
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, f -> f.disableRelocation());
        objects.addActorFactory("actor", Actor.class, Factory.class, f -> f.disableRelocation());
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private void createLocalTargets(
            systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime runtime) throws Exception {
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
    }

    private ZLinkRequestStage<String> submitLocal(
            systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime runtime,
            boolean actor,
            Duration timeout,
            boolean hold) {
        return actor
                ? runtime.actorClient()
                        .requestToActor("actor-a", new Probe(hold))
                        .timeout(timeout)
                        .submit(String.class)
                : runtime.route()
                        .requestToSpot("room-a", new Probe(hold))
                        .timeout(timeout)
                        .submit(String.class);
    }

    private ZLinkApplicationJobQueue applicationQueue(
            systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime runtime) throws Exception {
        var hostField = runtime.getClass().getDeclaredField("spots");
        hostField.setAccessible(true);
        var host = hostField.get(runtime);
        var queueField = host.getClass().getDeclaredField("applicationJobQueue");
        queueField.setAccessible(true);
        return (ZLinkApplicationJobQueue) queueField.get(host);
    }

    private void awaitCapacityWaiter(ZLinkApplicationJobQueue queue) {
        assertTimeoutPreemptively(
                Duration.ofSeconds(2),
                () -> {
                    while (queue.snapshot().capacityWaiters() == 0) Thread.sleep(1);
                });
    }

    private CompletableFuture<String> directRequest(
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot source,
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot target) {
        try (var part = systems.zlink.contracts.messaging.Message.from("probe")) {
            return source.requestToSpot(
                            RoutingId.from("local-deadline"),
                            target.spotId(),
                            target.lifecycleGeneration(),
                            null,
                            java.util.List.of(part),
                            Duration.ofMillis(100),
                            null)
                    .thenApply(
                            reply -> {
                                try (reply) {
                                    return reply.parts().getFirst().toUtf8String();
                                }
                            })
                    .toCompletableFuture();
        }
    }

    private static CompletableFuture<String> heldReply;

    public record Probe(boolean hold) {
        public Probe() {
            this(false);
        }
    }

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
            return request.hold() ? heldReply : CompletableFuture.completedFuture("reply");
        }
    }

    public static final class SpotHandler implements ZLinkSpotRequestHandler<Room, Probe, String> {
        public CompletionStage<String> handle(Room spot, Probe request) {
            handled.incrementAndGet();
            return request.hold() ? heldReply : CompletableFuture.completedFuture("reply");
        }
    }
}
