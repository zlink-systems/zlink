package systems.zlink.framework.runtime.actors;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Handler-local registration scope for deferred Actor membership transitions.
 */
final class ZLinkDeferredActorJoinScope {
    static final int MAX_JOIN_COUNT = 64;
    static final int MAX_REQUEST_BYTES = 1024 * 1024;
    static final int MAX_TOTAL_REQUEST_BYTES = 8 * 1024 * 1024;

    private static final Object LEGACY_RUNTIME_SCOPE = new Object();
    private static final ConcurrentMap<ActorScopeKey, State> ACTIVE_ACTOR_SCOPES =
        new ConcurrentHashMap<>();
    private static final java.util.Set<State> ACTIVE_HANDLER_SCOPES =
        ConcurrentHashMap.newKeySet();

    private ZLinkDeferredActorJoinScope() {
    }

    static Object legacyRuntimeScope() {
        return LEGACY_RUNTIME_SCOPE;
    }

    static State current() {
        return (State) systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentDeferredActorJoin();
    }

    static Scope enter(String actorId) {
        return enter(LEGACY_RUNTIME_SCOPE, actorId, actorId);
    }

    static Scope enter(
        Object runtimeScope,
        Object actorIncarnation,
        String actorId) {
        return enter(
            runtimeScope,
            actorIncarnation,
            actorId,
            candidate -> Objects.equals(actorId, candidate),
            true);
    }

    static Scope enterHandler(Predicate<String> actorAllowed) {
        return enterHandler(LEGACY_RUNTIME_SCOPE, actorAllowed);
    }

    static Scope enterHandler(
        Object runtimeScope,
        Predicate<String> actorAllowed) {
        return enter(
            runtimeScope,
            null,
            null,
            Objects.requireNonNull(actorAllowed, "actorAllowed"),
            false);
    }

    private static Scope enter(
        Object runtimeScope,
        Object actorIncarnation,
        String dispatchActorId,
        Predicate<String> actorAllowed,
        boolean actorScope) {
        ThreadLocal<Object> local = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.deferredActorJoinThreadLocal();
        Object previous = local.get();
        ActorScopeKey actorKey = actorScope
            ? new ActorScopeKey(
                Objects.requireNonNull(runtimeScope, "runtimeScope"),
                Objects.requireNonNull(actorIncarnation, "actorIncarnation"),
                Objects.requireNonNull(dispatchActorId, "dispatchActorId"))
            : null;
        State state = new State(runtimeScope, actorKey, actorAllowed);
        if (actorScope) {
            State active = ACTIVE_ACTOR_SCOPES.putIfAbsent(actorKey, state);
            if (active != null) {
                state = active;
            }
        }
        if (!actorScope) {
            ACTIVE_HANDLER_SCOPES.add(state);
        }
        local.set(state);
        return new Scope(local, previous, state, actorScope);
    }

    static void register(
        String actorId,
        int requestBytes,
        long deadlineNanos,
        Supplier<CompletionStage<Void>> operation) {
        register(
            LEGACY_RUNTIME_SCOPE,
            actorId,
            actorId,
            requestBytes,
            deadlineNanos,
            operation,
            null,
            () -> { });
    }

    static void registerWithActorBarrier(
        String actorId,
        int requestBytes,
        long deadlineNanos,
        Supplier<CompletionStage<Void>> operation,
        Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>>
            actorMailbox) {
        registerWithActorBarrier(
            LEGACY_RUNTIME_SCOPE,
            actorId,
            actorId,
            requestBytes,
            deadlineNanos,
            operation,
            actorMailbox,
            () -> { });
    }

    static void registerWithActorBarrier(
        Object runtimeScope,
        Object actorIncarnation,
        String actorId,
        int requestBytes,
        long deadlineNanos,
        Supplier<CompletionStage<Void>> operation,
        Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>>
            actorMailbox,
        Runnable releaseClaim) {
        register(
            runtimeScope,
            actorIncarnation,
            actorId,
            requestBytes,
            deadlineNanos,
            operation,
            Objects.requireNonNull(actorMailbox, "actorMailbox"),
            Objects.requireNonNull(releaseClaim, "releaseClaim"));
    }

    private static void register(
        Object runtimeScope,
        Object actorIncarnation,
        String actorId,
        int requestBytes,
        long deadlineNanos,
        Supplier<CompletionStage<Void>> operation,
        Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>>
            actorMailbox,
        Runnable releaseClaim) {
        ActorScopeKey actorKey = new ActorScopeKey(
            Objects.requireNonNull(runtimeScope, "runtimeScope"),
            Objects.requireNonNull(actorIncarnation, "actorIncarnation"),
            Objects.requireNonNull(actorId, "actorId"));
        State state = current();
        if (state != null && !state.matches(runtimeScope, actorKey, actorId)) {
            state = null;
        }
        if (state == null) {
            state = ACTIVE_ACTOR_SCOPES.get(actorKey);
        }
        if (state == null) {
            State matched = null;
            for (State candidate : ACTIVE_HANDLER_SCOPES) {
                if (!candidate.matches(runtimeScope, actorKey, actorId)) {
                    continue;
                }
                if (matched != null && matched != candidate) {
                    throw failure(
                        ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                        "Actor join defer matches more than one open handler scope");
                }
                matched = candidate;
            }
            state = matched;
        }
        if (state == null) {
            throw failure(
                ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                "Actor join defer is only valid in an open Framework handler scope");
        }
        synchronized (state) {
            if (state.sealed) {
                throw failure(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    "Actor join handler registration scope is closed");
            }
            if (!state.accepts(actorId)) {
                throw failure(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    "Actor join defer must target a local Actor allowed by the current handler");
            }
            if (requestBytes < 0 || requestBytes > MAX_REQUEST_BYTES) {
                throw failure(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    "Actor join request exceeds the 1 MiB encoded limit");
            }
            if (state.intents.size() >= MAX_JOIN_COUNT
                || state.requestBytes + requestBytes > MAX_TOTAL_REQUEST_BYTES) {
                throw failure(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    "Actor join registrations exceed the handler limit");
            }
            if (state.claimedActorIds.contains(actorId)) {
                throw failure(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "Actor already has a pending membership transition");
            }
            state.claimedActorIds.add(actorId);
            state.claimReleases.add(releaseClaim);
            state.requestBytes += requestBytes;
            if (actorMailbox == null) {
                state.intents.add(new Intent(deadlineNanos, operation, () -> { }));
                return;
            }
            CompletableFuture<Boolean> activation = new CompletableFuture<>();
            CompletionStage<Void> barrier;
            try {
                barrier = actorMailbox.apply(() -> activation.thenCompose(active ->
                    active
                        ? operation.get()
                        : CompletableFuture.completedFuture(null)));
            } catch (RuntimeException error) {
                int claimIndex = state.claimedActorIds.lastIndexOf(actorId);
                state.claimedActorIds.remove(claimIndex);
                Runnable removedRelease = state.claimReleases.remove(claimIndex);
                state.requestBytes -= requestBytes;
                removedRelease.run();
                throw error;
            }
            state.intents.add(new Intent(
                deadlineNanos,
                () -> {
                    activation.complete(true);
                    return barrier;
                },
                () -> activation.complete(false)));
        }
    }

    private static ZLinkFrameworkException failure(
        ZLinkFrameworkErrorKind kind,
        String message) {
        return new ZLinkFrameworkException(kind, message);
    }

    /**
     * Runs one intent's own activation and absorbs its outcome so a single
     * failing join can neither fail the handler's terminal reply nor stall
     * the intents registered after it (see the barrier-latch note above).
     * The join's own completion -- accepted, rejected or failed -- still
     * reaches the caller through {@code onJoinCompleted}; this only stops it
     * from also becoming the dispatch stage's outcome.
     */
    private static CompletionStage<Void> neutralize(Intent intent) {
        try {
            CompletionStage<Void> operation = intent.operation().get();
            return operation == null
                ? CompletableFuture.completedFuture(null)
                : operation.handle((ignored, error) -> null);
        } catch (RuntimeException error) {
            return CompletableFuture.completedFuture(null);
        }
    }

    static final class Scope implements AutoCloseable {
        private final ThreadLocal<Object> local;
        private final Object previous;
        private final State state;
        private final boolean actorScope;
        private boolean closed;
        private boolean finished;

        private Scope(
            ThreadLocal<Object> local,
            Object previous,
            State state,
            boolean actorScope) {
            this.local = local;
            this.previous = previous;
            this.state = state;
            this.actorScope = actorScope;
        }

        CompletionStage<Void> finish(
            CompletionStage<?> handler,
            Supplier<CompletionStage<Void>> failureObserver) {
            finished = true;
            synchronized (state) {
                if (state.finishStarted) {
                    return handler.thenApply(ignored -> null);
                }
                state.finishStarted = true;
            }
            return handler.handle((ignored, error) -> error)
                .thenCompose(error -> {
                    synchronized (state) {
                        state.sealed = true;
                    }
                    removeActiveScope();
                    if (error != null) {
                        state.discardIntents();
                        state.releaseClaims();
                        CompletionStage<Void> observed = failureObserver == null
                            ? CompletableFuture.completedFuture(null)
                            : failureObserver.get();
                        return observed.thenCompose(nothing ->
                            CompletableFuture.failedFuture(unwrap(error)));
                    }
                    // Ledger route-mesh-11.0.0 §2.3: the handler reply is already
                    // terminal once the handler itself succeeds, so a deferred
                    // join's own failure must never replace it (parity with the
                    // Node/.NET decoupled shape: Node swallows the activation
                    // error after capturing the handler result, .NET posts each
                    // join independently and reports failure only through
                    // onJoinCompleted). Each intent's own outcome is neutralized
                    // before the next intent runs -- not only after the whole
                    // chain -- because a barrier-registered intent already has
                    // its activation latch reserved on the target actor's queue
                    // at register() time; skipping straight to the end on a
                    // failure would leave that latch (and the target's queue)
                    // stuck forever instead of merely dropping one join.
                    CompletionStage<Void> tail =
                        CompletableFuture.completedFuture(null);
                    for (Intent intent : List.copyOf(state.intents)) {
                        tail = tail.thenCompose(nothing -> neutralize(intent));
                    }
                    tail.whenComplete((nothing, activationError) -> {
                        state.releaseClaims();
                    });
                    // Handler completion and deferred membership completion
                    // are separate terminals. Barrier-backed intents were
                    // reserved during registration, so activating their
                    // latches here is enough to place them before application
                    // turns that were already waiting on the Actor lane.
                    return CompletableFuture.completedFuture(null);
                });
        }

        @Override
        public void close() {
            if (closed) {
                return;
            }
            closed = true;
            if (!finished) {
                state.sealed = true;
                removeActiveScope();
                state.discardIntents();
                state.releaseClaims();
            }
            if (previous == null) {
                local.remove();
            } else {
                local.set(previous);
            }
        }

        private void removeActiveScope() {
            if (actorScope) {
                ACTIVE_ACTOR_SCOPES.remove(state.actorKey, state);
            } else {
                ACTIVE_HANDLER_SCOPES.remove(state);
            }
        }
    }

    static final class State {
        private final Object runtimeScope;
        private final ActorScopeKey actorKey;
        private final Predicate<String> actorAllowed;
        private final List<Intent> intents = new ArrayList<>();
        private final List<String> claimedActorIds = new ArrayList<>();
        private final List<Runnable> claimReleases = new ArrayList<>();
        private int requestBytes;
        private volatile boolean sealed;
        private boolean finishStarted;

        private State(
            Object runtimeScope,
            ActorScopeKey actorKey,
            Predicate<String> actorAllowed) {
            this.runtimeScope = Objects.requireNonNull(runtimeScope, "runtimeScope");
            this.actorKey = actorKey;
            this.actorAllowed = Objects.requireNonNull(actorAllowed, "actorAllowed");
        }

        private boolean accepts(String actorId) {
            return actorId != null && actorAllowed.test(actorId);
        }

        private boolean matches(
            Object candidateRuntimeScope,
            ActorScopeKey candidateActorKey,
            String actorId) {
            if (runtimeScope != candidateRuntimeScope || !accepts(actorId)) {
                return false;
            }
            return actorKey == null || actorKey.equals(candidateActorKey);
        }

        private void releaseClaims() {
            for (Runnable releaseClaim : List.copyOf(claimReleases)) {
                releaseClaim.run();
            }
            claimReleases.clear();
            claimedActorIds.clear();
            intents.clear();
        }

        private void discardIntents() {
            for (Intent intent : List.copyOf(intents)) {
                intent.discard.run();
            }
        }
    }

    private record Intent(
        long deadlineNanos,
        Supplier<CompletionStage<Void>> operation,
        Runnable discard) {
    }

    private record ActorScopeKey(
        Object runtimeScope,
        Object actorIncarnation,
        String actorId) {
    }

    private static Throwable unwrap(Throwable error) {
        return error instanceof CompletionException && error.getCause() != null
            ? error.getCause()
            : error;
    }
}
