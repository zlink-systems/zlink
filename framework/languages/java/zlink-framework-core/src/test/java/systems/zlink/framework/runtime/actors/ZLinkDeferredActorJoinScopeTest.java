package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

final class ZLinkDeferredActorJoinScopeTest {
    private static final int BULK_JOIN_COUNT = 65;
    private static final int BULK_REQUEST_BYTES = 129 * 1024;
    private static final String BULK_TARGET_SPOT_ID = "bulk-target";

    @Test
    void oneHandlerCompletes65ActualDeferredActorJoinsWhoseRequestsTotalMoreThan8MiB()
            throws Exception {
        BulkActorFactory.reset();
        BulkTargetSpot.reset();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("bulk-join");
        node.listen("inproc://bulk-deferred-join-" + System.nanoTime())
                .setRoutingId(RoutingId.from("bulk-deferred-join"));
        var objects = node.objects().server();
        objects.addEntrySpot(BulkEntrySpot.class);
        objects.addSpotFactory(
                "bulk-target", BulkTargetSpot.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "bulk-actor",
                BulkActor.class,
                BulkActorFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                    .getOrCreate(BULK_TARGET_SPOT_ID, "bulk-target")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            for (int index = 0; index < BULK_JOIN_COUNT; index++) {
                runtime.actorManager()
                        .create("bulk-actor-" + index, "bulk-actor")
                        .submit()
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
            }

            ZLinkActorRuntime actorRuntime = (ZLinkActorRuntime) runtime.actorManager();
            String request = "x".repeat(BULK_REQUEST_BYTES);
            ZLinkDeferredActorJoinHandlerScope.run(
                            actorRuntime.deferredJoinRuntimeScope(),
                            actorId -> actorId.startsWith("bulk-actor-"),
                            () -> {
                                for (int index = 0; index < BULK_JOIN_COUNT; index++) {
                                    BulkActorFactory.actor("bulk-actor-" + index)
                                            .context()
                                            .joinSpot(BULK_TARGET_SPOT_ID, request)
                                            .defer();
                                }
                                return CompletableFuture.completedFuture(null);
                            })
                    .toCompletableFuture()
                    .join();

            BulkActorFactory.allCompleted().get(10, TimeUnit.SECONDS);
            assertEquals(BULK_JOIN_COUNT, BulkTargetSpot.joinCount());
            assertTrue(BulkTargetSpot.requestBytes() > 8L * 1024 * 1024);
        }
    }

    @Test
    void deferredJoinUsesOnlyTheTimeLeftAfterItsHandlerBarrier() {
        long deadline = System.nanoTime() + Duration.ofSeconds(1).toNanos();

        Duration remaining = ZLinkActorSpotJoinCall.remainingTimeout(deadline);

        assertTrue(remaining != null);
        assertTrue(remaining.compareTo(Duration.ofSeconds(1)) <= 0);
        assertTrue(ZLinkActorSpotJoinCall.remainingTimeout(System.nanoTime() - 1) == null);
    }

    @Test
    void actorScopeDoesNotCrossRuntimeOrActorIncarnation() {
        Object runtime = new Object();
        Object incarnation = new Object();

        try (ZLinkDeferredActorJoinScope.Scope ignored =
                ZLinkDeferredActorJoinScope.enter(runtime, incarnation, "actor-a")) {
            ZLinkFrameworkException otherRuntime =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                            new Object(),
                                            incarnation,
                                            "actor-a",
                                            Long.MAX_VALUE,
                                            () -> CompletableFuture.completedFuture(null),
                                            operation -> operation.get(),
                                            () -> {}));
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, otherRuntime.kind());

            ZLinkFrameworkException replacementIncarnation =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                            runtime,
                                            new Object(),
                                            "actor-a",
                                            Long.MAX_VALUE,
                                            () -> CompletableFuture.completedFuture(null),
                                            operation -> operation.get(),
                                            () -> {}));
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, replacementIncarnation.kind());
        }
    }

    @Test
    void spotHandlerScopeDoesNotCrossRuntime() {
        Object handlerRuntime = new Object();

        CompletionStage<Void> handler =
                ZLinkDeferredActorJoinHandlerScope.run(
                        handlerRuntime,
                        actorId -> actorId.equals("actor-a"),
                        () -> {
                            ZLinkFrameworkException otherRuntime =
                                    assertThrows(
                                            ZLinkFrameworkException.class,
                                            () ->
                                                    ZLinkDeferredActorJoinScope
                                                            .registerWithActorBarrier(
                                                                    new Object(),
                                                                    new Object(),
                                                                    "actor-a",
                                                                    Long.MAX_VALUE,
                                                                    () ->
                                                                            CompletableFuture
                                                                                    .completedFuture(
                                                                                            null),
                                                                    operation -> operation.get(),
                                                                    () -> {}));
                            assertEquals(
                                    ZLinkFrameworkErrorKind.NOT_CONFIGURED, otherRuntime.kind());
                            return CompletableFuture.completedFuture(null);
                        });

        handler.toCompletableFuture().join();
    }

    @Test
    void activatesOnlyAfterNormalHandlerTerminal() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        serials.runTurn(
                        "actor-a",
                        () -> {
                            order.add("handler");
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join");
                                        return CompletableFuture.completedFuture(null);
                                    });
                            order.add("terminal");
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();

        assertEquals(List.of("handler", "terminal", "join"), order);
    }

    @Test
    void awaitedJavaContinuationUsesTheOpenActorScope() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> awaited = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        CompletableFuture<Void> turn =
                serials.runTurn(
                                "actor-a",
                                () ->
                                        awaited.thenRunAsync(
                                                () -> {
                                                    order.add("continuation");
                                                    ZLinkDeferredActorJoinScope.register(
                                                            "actor-a",
                                                            Long.MAX_VALUE,
                                                            () -> {
                                                                order.add("join");
                                                                return CompletableFuture
                                                                        .completedFuture(null);
                                                            });
                                                }))
                        .toCompletableFuture();

        assertTrue(order.isEmpty());
        awaited.complete(null);
        turn.join();
        assertEquals(List.of("continuation", "join"), order);
    }

    @Test
    void discardsEveryInactiveIntentWhenHandlerFails() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                serials.runTurn(
                                                "actor-a",
                                                () -> {
                                                    ZLinkDeferredActorJoinScope.register(
                                                            "actor-a",
                                                            Long.MAX_VALUE,
                                                            () -> {
                                                                order.add("join");
                                                                return CompletableFuture
                                                                        .completedFuture(null);
                                                            });
                                                    return CompletableFuture.failedFuture(
                                                            new IllegalStateException(
                                                                    "handler failed"));
                                                })
                                        .toCompletableFuture()
                                        .join());

        assertTrue(failure.getCause() instanceof IllegalStateException);
        assertTrue(order.isEmpty());

        serials.runTurn(
                        "actor-a",
                        () -> {
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> CompletableFuture.completedFuture(null));
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
    }

    @Test
    void rejectsDetachedWrongActorAndDuplicateClaims() {
        ZLinkFrameworkException detached =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                ZLinkDeferredActorJoinScope.register(
                                        "actor-a",
                                        Long.MAX_VALUE,
                                        () -> CompletableFuture.completedFuture(null)));
        assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, detached.kind());

        try (ZLinkDeferredActorJoinScope.Scope scope =
                ZLinkDeferredActorJoinScope.enter("actor-a")) {
            ZLinkFrameworkException wrongActor =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    ZLinkDeferredActorJoinScope.register(
                                            "actor-b",
                                            Long.MAX_VALUE,
                                            () -> CompletableFuture.completedFuture(null)));
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, wrongActor.kind());

            ZLinkDeferredActorJoinScope.register(
                    "actor-a", Long.MAX_VALUE, () -> CompletableFuture.completedFuture(null));
            ZLinkFrameworkException moving =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    ZLinkDeferredActorJoinScope.register(
                                            "actor-a",
                                            Long.MAX_VALUE,
                                            () -> CompletableFuture.completedFuture(null)));
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, moving.kind());
            scope.finish(CompletableFuture.completedFuture(null), null)
                    .toCompletableFuture()
                    .join();
        }
    }

    @Test
    void userOrEntryHandlerRegistersSeveralMemberActorsInCallOrder() {
        List<String> order = new ArrayList<>();

        ZLinkDeferredActorJoinHandlerScope.run(
                        actorId -> actorId.equals("actor-a") || actorId.equals("actor-b"),
                        () -> {
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("actor-a");
                                        return CompletableFuture.completedFuture(null);
                                    });
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-b",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("actor-b");
                                        return CompletableFuture.completedFuture(null);
                                    });
                            order.add("handler-terminal");
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();

        assertEquals(List.of("handler-terminal", "actor-a", "actor-b"), order);
    }

    @Test
    void userOrEntryHandlerRejectsAnActorOutsideItsMembershipProjection() {
        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkDeferredActorJoinHandlerScope.run(
                                                actorId -> actorId.equals("actor-a"),
                                                () -> {
                                                    ZLinkDeferredActorJoinScope.register(
                                                            "actor-b",
                                                            Long.MAX_VALUE,
                                                            () ->
                                                                    CompletableFuture
                                                                            .completedFuture(null));
                                                    return CompletableFuture.completedFuture(null);
                                                })
                                        .toCompletableFuture()
                                        .join());

        assertTrue(failure.getCause() instanceof ZLinkFrameworkException);
        assertEquals(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                ((ZLinkFrameworkException) failure.getCause()).kind());
    }

    @Test
    void spotHandlerReservesEachActorMailboxBarrierBeforeItsTerminal() {
        List<String> order = new ArrayList<>();
        AtomicReference<Supplier<CompletionStage<Void>>> queued = new AtomicReference<>();

        CompletionStage<Void> handler =
                ZLinkDeferredActorJoinHandlerScope.run(
                        actorId -> actorId.equals("actor-a"),
                        () -> {
                            ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join");
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    operation -> {
                                        order.add("barrier-reserved");
                                        queued.set(operation);
                                        return CompletableFuture.completedFuture(null)
                                                .thenCompose(ignored -> operation.get());
                                    });
                            order.add("handler-terminal");
                            return CompletableFuture.completedFuture(null);
                        });

        handler.toCompletableFuture().join();
        assertTrue(queued.get() != null);
        assertEquals(List.of("barrier-reserved", "handler-terminal", "join"), order);
    }

    @Test
    void aFailingDirectJoinStillRunsLaterJoinsAndTheHandlerReplySurvives() {
        List<String> order = new ArrayList<>();

        CompletionStage<String> handler =
                ZLinkDeferredActorJoinHandlerScope.run(
                        actorId -> actorId.equals("actor-a") || actorId.equals("actor-b"),
                        () -> {
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join-a");
                                        return CompletableFuture.failedFuture(
                                                new IllegalStateException("join-a failed"));
                                    });
                            ZLinkDeferredActorJoinScope.register(
                                    "actor-b",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join-b");
                                        return CompletableFuture.completedFuture(null);
                                    });
                            order.add("handler-terminal");
                            return CompletableFuture.completedFuture("reply-value");
                        });

        // Ledger route-mesh-11.0.0 §2.3 decoupling axis: the handler's own
        // terminal reply must survive a later deferred join's failure, and
        // that failure must not stop registration-order siblings from running.
        assertEquals("reply-value", handler.toCompletableFuture().join());
        assertEquals(List.of("handler-terminal", "join-a", "join-b"), order);
    }

    @Test
    void aFailingBarrierJoinStillActivatesLaterRegisteredJoinsAndDoesNotWedgeTheirQueue() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> handler =
                ZLinkDeferredActorJoinHandlerScope.run(
                        actorId -> actorId.equals("actor-a") || actorId.equals("actor-b"),
                        () -> {
                            ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join-a");
                                        return CompletableFuture.failedFuture(
                                                new IllegalStateException(
                                                        "join-a transport failure"));
                                    },
                                    operation -> serials.enqueueBarrier("actor-a", operation));
                            ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                    "actor-b",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join-b");
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    operation -> serials.enqueueBarrier("actor-b", operation));
                            order.add("handler-terminal");
                            return CompletableFuture.completedFuture(null);
                        });

        // The handler terminal is independent from deferred membership work:
        // join-a's own failure never reaches it or replaces its outcome.
        handler.toCompletableFuture().join();

        // If join-a's failure had short-circuited the registration-order
        // chain, actor-b's activation latch would never fire and its queue
        // would stay wedged behind that barrier turn forever. Prove it did
        // not: a turn enqueued after the barrier still runs.
        List<String> followUp = new ArrayList<>();
        serials.enqueue(
                        serials.prepare("actor-b"),
                        () -> {
                            followUp.add("actor-b-next-turn");
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();

        assertEquals(List.of("handler-terminal", "join-a", "join-b"), order);
        assertEquals(List.of("actor-b-next-turn"), followUp);
    }

    @Test
    void sameActorJoinRunsInReservedBarrierWithoutHoldingHandlerTerminal() throws Exception {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> joinStarted = new CompletableFuture<>();
        CompletableFuture<Void> joinCompletion = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> handler =
                serials.enqueue(
                        serials.prepare("actor-a"),
                        () -> {
                            ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                                    "actor-a",
                                    Long.MAX_VALUE,
                                    () -> {
                                        order.add("join");
                                        joinStarted.complete(null);
                                        return joinCompletion;
                                    },
                                    operation -> serials.enqueueBarrier("actor-a", operation));
                            order.add("handler-terminal");
                            return CompletableFuture.completedFuture(null);
                        });
        CompletionStage<Void> queuedApplication =
                serials.enqueue(
                        serials.prepare("actor-a"),
                        () -> {
                            order.add("queued-application");
                            return CompletableFuture.completedFuture(null);
                        });

        handler.toCompletableFuture().join();
        joinStarted.get(2, TimeUnit.SECONDS);
        assertFalse(queuedApplication.toCompletableFuture().isDone());
        assertEquals(List.of("handler-terminal", "join"), order);

        joinCompletion.complete(null);
        queuedApplication.toCompletableFuture().join();
        assertEquals(List.of("handler-terminal", "join", "queued-application"), order);
    }

    public static final class BulkActor implements ZLinkActor {
        private final ZLinkActorContext context;

        public BulkActor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            try {
                assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, completion);
                BulkActorFactory.joinCompleted();
            } catch (RuntimeException | AssertionError error) {
                BulkActorFactory.joinFailed(error);
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class BulkActorFactory implements ZLinkActorFactory {
        private static final Map<String, BulkActor> ACTORS = new ConcurrentHashMap<>();
        private static final AtomicInteger COMPLETED = new AtomicInteger();
        private static CompletableFuture<Void> allCompleted = new CompletableFuture<>();

        static void reset() {
            ACTORS.clear();
            COMPLETED.set(0);
            allCompleted = new CompletableFuture<>();
        }

        static BulkActor actor(String actorId) {
            return ACTORS.get(actorId);
        }

        static CompletableFuture<Void> allCompleted() {
            return allCompleted;
        }

        static void joinCompleted() {
            if (COMPLETED.incrementAndGet() == BULK_JOIN_COUNT) {
                allCompleted.complete(null);
            }
        }

        static void joinFailed(Throwable error) {
            allCompleted.completeExceptionally(error);
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            BulkActor actor = new BulkActor(context);
            ACTORS.put(context.actorId(), actor);
            return CompletableFuture.completedFuture(actor);
        }
    }

    public static final class BulkEntrySpot implements ZLinkEntrySpot<BulkActor> {
        private final ZLinkEntrySpotContext context;

        public BulkEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(BulkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(BulkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class BulkTargetSpot implements ZLinkSpot<BulkActor> {
        private static final AtomicInteger JOIN_COUNT = new AtomicInteger();
        private static final AtomicLong REQUEST_BYTES = new AtomicLong();
        private final ZLinkSpotContext context;

        public BulkTargetSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        static void reset() {
            JOIN_COUNT.set(0);
            REQUEST_BYTES.set(0);
        }

        static int joinCount() {
            return JOIN_COUNT.get();
        }

        static long requestBytes() {
            return REQUEST_BYTES.get();
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            String payload = request.decode(String.class);
            REQUEST_BYTES.addAndGet(payload.length());
            JOIN_COUNT.incrementAndGet();
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(BulkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(BulkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
