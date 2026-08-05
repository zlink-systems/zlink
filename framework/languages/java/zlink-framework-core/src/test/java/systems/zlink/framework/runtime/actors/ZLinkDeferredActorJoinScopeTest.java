package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkDeferredActorJoinScopeTest {
    @Test
    void deferredJoinUsesOnlyTheTimeLeftAfterItsHandlerBarrier() {
        long deadline = System.nanoTime() + Duration.ofSeconds(1).toNanos();

        Duration remaining =
            ZLinkActorSpotJoinCall.remainingTimeout(deadline);

        assertTrue(remaining != null);
        assertTrue(remaining.compareTo(Duration.ofSeconds(1)) <= 0);
        assertTrue(ZLinkActorSpotJoinCall.remainingTimeout(
            System.nanoTime() - 1) == null);
    }

    @Test
    void actorScopeDoesNotCrossRuntimeOrActorIncarnation() {
        Object runtime = new Object();
        Object incarnation = new Object();

        try (ZLinkDeferredActorJoinScope.Scope ignored =
                 ZLinkDeferredActorJoinScope.enter(
                     runtime,
                     incarnation,
                     "actor-a")) {
            ZLinkFrameworkException otherRuntime = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    new Object(),
                    incarnation,
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> CompletableFuture.completedFuture(null),
                    operation -> operation.get(),
                    () -> { }));
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                otherRuntime.kind());

            ZLinkFrameworkException replacementIncarnation = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    runtime,
                    new Object(),
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> CompletableFuture.completedFuture(null),
                    operation -> operation.get(),
                    () -> { }));
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                replacementIncarnation.kind());
        }
    }

    @Test
    void spotHandlerScopeDoesNotCrossRuntime() {
        Object handlerRuntime = new Object();

        CompletionStage<Void> handler = ZLinkDeferredActorJoinHandlerScope.run(
            handlerRuntime,
            actorId -> actorId.equals("actor-a"),
            () -> {
                ZLinkFrameworkException otherRuntime = assertThrows(
                    ZLinkFrameworkException.class,
                    () -> ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                        new Object(),
                        new Object(),
                        "actor-a",
                        0,
                        Long.MAX_VALUE,
                        () -> CompletableFuture.completedFuture(null),
                        operation -> operation.get(),
                        () -> { }));
                assertEquals(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    otherRuntime.kind());
                return CompletableFuture.completedFuture(null);
            });

        handler.toCompletableFuture().join();
    }

    @Test
    void activatesOnlyAfterNormalHandlerTerminal() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        serials.runTurn("actor-a", () -> {
            order.add("handler");
            ZLinkDeferredActorJoinScope.register(
                "actor-a",
                4,
                Long.MAX_VALUE,
                () -> {
                    order.add("join");
                    return CompletableFuture.completedFuture(null);
                });
            order.add("terminal");
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture().join();

        assertEquals(List.of("handler", "terminal", "join"), order);
    }

    @Test
    void awaitedJavaContinuationUsesTheOpenActorScope() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> awaited = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        CompletableFuture<Void> turn = serials.runTurn(
            "actor-a",
            () -> awaited.thenRunAsync(() -> {
                order.add("continuation");
                ZLinkDeferredActorJoinScope.register(
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("join");
                        return CompletableFuture.completedFuture(null);
                    });
            })).toCompletableFuture();

        assertTrue(order.isEmpty());
        awaited.complete(null);
        turn.join();
        assertEquals(List.of("continuation", "join"), order);
    }

    @Test
    void discardsEveryInactiveIntentWhenHandlerFails() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> serials.runTurn("actor-a", () -> {
                ZLinkDeferredActorJoinScope.register(
                    "actor-a",
                    4,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("join");
                        return CompletableFuture.completedFuture(null);
                    });
                return CompletableFuture.failedFuture(
                    new IllegalStateException("handler failed"));
            }).toCompletableFuture().join());

        assertTrue(failure.getCause() instanceof IllegalStateException);
        assertTrue(order.isEmpty());

        serials.runTurn("actor-a", () -> {
            ZLinkDeferredActorJoinScope.register(
                "actor-a",
                0,
                Long.MAX_VALUE,
                () -> CompletableFuture.completedFuture(null));
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture().join();
    }

    @Test
    void rejectsDetachedWrongActorOversizedAndDuplicateClaims() {
        ZLinkFrameworkException detached = assertThrows(
            ZLinkFrameworkException.class,
            () -> ZLinkDeferredActorJoinScope.register(
                "actor-a",
                0,
                Long.MAX_VALUE,
                () -> CompletableFuture.completedFuture(null)));
        assertEquals(
            ZLinkFrameworkErrorKind.NOT_CONFIGURED,
            detached.kind());

        try (ZLinkDeferredActorJoinScope.Scope scope =
                 ZLinkDeferredActorJoinScope.enter("actor-a")) {
            ZLinkFrameworkException wrongActor = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkDeferredActorJoinScope.register(
                    "actor-b",
                    0,
                    Long.MAX_VALUE,
                    () -> CompletableFuture.completedFuture(null)));
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                wrongActor.kind());

            ZLinkFrameworkException oversized = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkDeferredActorJoinScope.register(
                    "actor-a",
                    ZLinkDeferredActorJoinScope.MAX_REQUEST_BYTES + 1,
                    Long.MAX_VALUE,
                    () -> CompletableFuture.completedFuture(null)));
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                oversized.kind());

            ZLinkDeferredActorJoinScope.register(
                "actor-a",
                0,
                Long.MAX_VALUE,
                () -> CompletableFuture.completedFuture(null));
            ZLinkFrameworkException moving = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkDeferredActorJoinScope.register(
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> CompletableFuture.completedFuture(null)));
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, moving.kind());
            scope.finish(
                CompletableFuture.completedFuture(null),
                null).toCompletableFuture().join();
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
                    2,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("actor-a");
                        return CompletableFuture.completedFuture(null);
                    });
                ZLinkDeferredActorJoinScope.register(
                    "actor-b",
                    2,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("actor-b");
                        return CompletableFuture.completedFuture(null);
                    });
                order.add("handler-terminal");
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture().join();

        assertEquals(
            List.of("handler-terminal", "actor-a", "actor-b"),
            order);
    }

    @Test
    void userOrEntryHandlerRejectsAnActorOutsideItsMembershipProjection() {
        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> ZLinkDeferredActorJoinHandlerScope.run(
                actorId -> actorId.equals("actor-a"),
                () -> {
                    ZLinkDeferredActorJoinScope.register(
                        "actor-b",
                        0,
                        Long.MAX_VALUE,
                        () -> CompletableFuture.completedFuture(null));
                    return CompletableFuture.completedFuture(null);
                }).toCompletableFuture().join());

        assertTrue(failure.getCause() instanceof ZLinkFrameworkException);
        assertEquals(
            ZLinkFrameworkErrorKind.NOT_CONFIGURED,
            ((ZLinkFrameworkException) failure.getCause()).kind());
    }

    @Test
    void spotHandlerReservesEachActorMailboxBarrierBeforeItsTerminal() {
        List<String> order = new ArrayList<>();
        java.util.concurrent.atomic.AtomicReference<
            java.util.function.Supplier<CompletionStage<Void>>> queued =
            new java.util.concurrent.atomic.AtomicReference<>();

        CompletionStage<Void> handler = ZLinkDeferredActorJoinHandlerScope.run(
            actorId -> actorId.equals("actor-a"),
            () -> {
                ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    "actor-a",
                    0,
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
        assertEquals(
            List.of("barrier-reserved", "handler-terminal", "join"),
            order);
    }

    @Test
    void aFailingDirectJoinStillRunsLaterJoinsAndTheHandlerReplySurvives() {
        List<String> order = new ArrayList<>();

        CompletionStage<String> handler = ZLinkDeferredActorJoinHandlerScope.run(
            actorId -> actorId.equals("actor-a") || actorId.equals("actor-b"),
            () -> {
                ZLinkDeferredActorJoinScope.register(
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("join-a");
                        return CompletableFuture.failedFuture(
                            new IllegalStateException("join-a failed"));
                    });
                ZLinkDeferredActorJoinScope.register(
                    "actor-b",
                    0,
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
        assertEquals(
            List.of("handler-terminal", "join-a", "join-b"),
            order);
    }

    @Test
    void aFailingBarrierJoinStillActivatesLaterRegisteredJoinsAndDoesNotWedgeTheirQueue() {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> handler = ZLinkDeferredActorJoinHandlerScope.run(
            actorId -> actorId.equals("actor-a") || actorId.equals("actor-b"),
            () -> {
                ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    "actor-a",
                    0,
                    Long.MAX_VALUE,
                    () -> {
                        order.add("join-a");
                        return CompletableFuture.failedFuture(
                            new IllegalStateException("join-a transport failure"));
                    },
                    operation -> serials.enqueueBarrier("actor-a", operation));
                ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    "actor-b",
                    0,
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
        serials.enqueue(serials.prepare("actor-b"), () -> {
                followUp.add("actor-b-next-turn");
                return CompletableFuture.completedFuture(null);
            })
            .toCompletableFuture()
            .join();

        assertEquals(
            List.of("handler-terminal", "join-a", "join-b"),
            order);
        assertEquals(List.of("actor-b-next-turn"), followUp);
    }

    @Test
    void sameActorJoinRunsInReservedBarrierWithoutHoldingHandlerTerminal()
        throws Exception {
        ZLinkActorDispatchSerials serials = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> joinStarted = new CompletableFuture<>();
        CompletableFuture<Void> joinCompletion = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> handler = serials.enqueue(
            serials.prepare("actor-a"),
            () -> {
                ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                    "actor-a",
                    0,
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
        CompletionStage<Void> queuedApplication = serials.enqueue(
            serials.prepare("actor-a"),
            () -> {
                order.add("queued-application");
                return CompletableFuture.completedFuture(null);
            });

        handler.toCompletableFuture().join();
        joinStarted.get(2, java.util.concurrent.TimeUnit.SECONDS);
        assertFalse(queuedApplication.toCompletableFuture().isDone());
        assertEquals(List.of("handler-terminal", "join"), order);

        joinCompletion.complete(null);
        queuedApplication.toCompletableFuture().join();
        assertEquals(
            List.of("handler-terminal", "join", "queued-application"),
            order);
    }
}
