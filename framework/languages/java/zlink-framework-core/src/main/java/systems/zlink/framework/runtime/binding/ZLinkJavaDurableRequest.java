package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.ConnectionAttempt;
import systems.zlink.contracts.sockets.ConnectionAttemptState;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerAdmissionExpectation;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerRouteObservation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry.Operation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRequestProgress;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.stream.Collectors;

/** Owns connection admission for one registered remote request. */
public final class ZLinkJavaDurableRequest<T> {
    public static Throwable requestFailure(Throwable failure) {
        return ZLinkJavaRawMeshNode.requestFailure(failure);
    }

    @FunctionalInterface
    public interface Submit<T> {
        CompletionStage<?> apply(
                List<byte[]> frames,
                Duration remaining,
                CompletableFuture<Void> admission,
                BiConsumer<T, Throwable> terminal);
    }

    @FunctionalInterface
    interface ProgressSubmit<T> {
        CompletionStage<?> apply(
                List<byte[]> frames,
                Duration remaining,
                CompletableFuture<Void> admission,
                BiConsumer<T, Throwable> terminal,
                ZLinkServiceRequestProgress progress);
    }

    private static final CompletionStage<Void> SUBMITTING = CompletableFuture.completedStage(null);
    private static final Logger LOGGER = Logger.getLogger(ZLinkJavaDurableRequest.class.getName());
    private final Supplier<List<byte[]>> prepare;
    private final ProgressSubmit<T> submit;
    private final Supplier<PeerRouteObservation> route;
    private final Function<Runnable, CompletionStage<Void>> owner;
    private final ZLinkServiceOperationRegistry operations;
    private final Operation<?> caller;
    private final BiConsumer<T, Throwable> terminal;
    private final CompletableFuture<Void> admission;
    private final Supplier<List<ConnectionAttempt>> connections;
    private Map<Long, Long> baseline;
    private PeerAdmissionExpectation baselineExpectation;
    private CompletionStage<?> pending;
    private AutoCloseable observation;
    private Runnable abandon = () -> {};

    private ZLinkJavaDurableRequest(
            Supplier<List<byte[]>> prepare,
            ProgressSubmit<T> submit,
            Supplier<PeerRouteObservation> route,
            Function<Runnable, CompletionStage<Void>> owner,
            ZLinkServiceOperationRegistry operations,
            Operation<?> caller,
            BiConsumer<T, Throwable> terminal,
            CompletableFuture<Void> admission,
            Supplier<List<ConnectionAttempt>> connections) {
        this.connections = connections;
        this.admission = admission;
        this.prepare = prepare;
        this.submit = submit;
        this.route = route;
        this.owner = owner;
        this.operations = operations;
        this.caller = caller;
        this.terminal = terminal;
    }

    public static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations) {
        return request(
                prepare,
                submit,
                route,
                observe,
                owner,
                timeout,
                operations,
                new CompletableFuture<>());
    }

    static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            CompletableFuture<Void> requestAdmission) {
        return request(
                prepare,
                submit,
                route,
                observe,
                owner,
                timeout,
                operations,
                requestAdmission,
                List::of);
    }

    static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            CompletableFuture<Void> requestAdmission,
            Supplier<List<ConnectionAttempt>> connections) {
        return request(
                prepare,
                (frames, remaining, admission, terminal, progress) ->
                        submit.apply(frames, remaining, admission, terminal),
                route,
                observe,
                owner,
                timeout,
                operations,
                requestAdmission,
                connections,
                ZLinkServiceOperationRegistry.prepareOperation(ZLinkServiceOperationIds.next()));
    }

    static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            ProgressSubmit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            CompletableFuture<Void> requestAdmission,
            Supplier<List<ConnectionAttempt>> connections,
            Operation<T> caller) {
        if (requestAdmission != null)
            caller.whenComplete(
                    (reply, failure) -> {
                        if (failure == null) requestAdmission.complete(null);
                        else requestAdmission.completeExceptionally(failure);
                    });
        operations.<T>submitTerminal(
                caller.id(),
                timeout,
                terminal -> {
                    attempt(
                            prepare,
                            submit,
                            route,
                            connections,
                            observe,
                            owner,
                            operations,
                            caller,
                            terminal,
                            requestAdmission,
                            () -> {});
                    return null;
                },
                ignored -> {},
                caller);
        return caller;
    }

    static <T> CompletionStage<?> attempt(
            Supplier<List<byte[]>> prepare,
            ProgressSubmit<T> submit,
            Supplier<PeerRouteObservation> route,
            Supplier<List<ConnectionAttempt>> connections,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            ZLinkServiceOperationRegistry operations,
            Operation<?> caller,
            BiConsumer<T, Throwable> terminal,
            CompletableFuture<Void> admission,
            Runnable abandon) {
        // A new request reads the observer's published route without entering its lane.
        // Only requests that need to wait allocate an observer and capture failure history.
        if (route.get().ready()) {
            if (submit(prepare, submit, operations, caller, terminal, admission) != null)
                return caller;
            if (!operations.isPending(caller.id())) {
                abandon.run();
                return caller;
            }
        }
        return waitForAdmission(
                prepare,
                submit,
                route,
                connections,
                observe,
                owner,
                operations,
                caller,
                terminal,
                admission,
                abandon);
    }

    private static <T> CompletionStage<?> waitForAdmission(
            Supplier<List<byte[]>> prepare,
            ProgressSubmit<T> submit,
            Supplier<PeerRouteObservation> route,
            Supplier<List<ConnectionAttempt>> connections,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            ZLinkServiceOperationRegistry operations,
            Operation<?> caller,
            BiConsumer<T, Throwable> terminal,
            CompletableFuture<Void> admission,
            Runnable abandon) {
        var request =
                new ZLinkJavaDurableRequest<>(
                        prepare,
                        submit,
                        route,
                        owner,
                        operations,
                        caller,
                        terminal,
                        admission,
                        connections);
        request.abandon = abandon;
        caller.whenComplete((reply, failure) -> request.post(request::release));
        request.post(
                () -> {
                    if (!operations.isPending(caller.id())) return;
                    request.attempt();
                    if (!operations.isPending(caller.id()) || request.pending != null) return;
                    request.observation =
                            observe.apply(
                                    () -> request.post(request::attempt),
                                    () -> request.post(request::endTarget));
                    if (!operations.isPending(caller.id())) request.release();
                    else request.attempt();
                });
        return caller;
    }

    private void post(Runnable action) {
        try {
            owner.apply(action)
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null)
                                    operations.completeExceptionally(caller.id(), failure);
                            });
        } catch (RuntimeException failure) {
            operations.completeExceptionally(caller.id(), failure);
        }
    }

    private void attempt() {
        if (!operations.isPending(caller.id()) || pending != null) return;
        var currentRoute = route.get();
        var current =
                baseline == null && currentRoute.ready()
                        ? List.<ConnectionAttempt>of()
                        : connections.get();
        var expectation = currentRoute.expectation();
        if (baselineExpectation != null
                && (expectation == null
                        || baselineExpectation.lifecycleGeneration()
                                != expectation.lifecycleGeneration()
                        || !baselineExpectation.endpoint().equals(expectation.endpoint())
                        || !baselineExpectation
                                .securityIdentity()
                                .equals(expectation.securityIdentity()))) {
            endTarget();
            return;
        }
        if (baseline != null) {
            for (var previous : baseline.entrySet()) {
                if (current.stream()
                        .noneMatch(
                                attempt ->
                                        attempt.intentId() == previous.getKey()
                                                && attempt.lastFailureId()
                                                        == previous.getValue())) {
                    endTarget();
                    return;
                }
            }
            if (baseline.isEmpty() && !current.isEmpty()) {
                if (current.stream().anyMatch(attempt -> attempt.lastFailureId() != 0)) {
                    endTarget();
                    return;
                }
            }
        }
        // Only a new request with an already admitted selected route bypasses past failures.
        if (!currentRoute.ready()) {
            if (baselineExpectation == null) baselineExpectation = expectation;
            if (currentRoute.selectedGeneration() == 0
                    && current.stream()
                            .anyMatch(
                                    attempt ->
                                            attempt.state()
                                                    == ConnectionAttemptState.ATTEMPT_FAILED)) {
                endTarget();
                return;
            }
            if (baseline == null || baseline.isEmpty())
                baseline =
                        current.stream()
                                .collect(
                                        Collectors.toUnmodifiableMap(
                                                ConnectionAttempt::intentId,
                                                ConnectionAttempt::lastFailureId));
            return;
        }
        // Mark the operation before submit: a synchronous terminal must not abandon its frames.
        pending = SUBMITTING;
        pending = submit(prepare, submit, operations, caller, terminal, admission);
        if (pending == null && !operations.isPending(caller.id())) release();
    }

    private static <T> CompletionStage<?> submit(
            Supplier<List<byte[]>> prepare,
            ProgressSubmit<T> submit,
            ZLinkServiceOperationRegistry operations,
            Operation<?> caller,
            BiConsumer<T, Throwable> terminal,
            CompletableFuture<Void> admission) {
        Duration remaining = operations.remainingTimeout(caller.id());
        if (remaining.isZero() || !operations.isPending(caller.id())) return null;
        var progress = caller.requestProgress();
        progress.peerAdmitted();
        try {
            List<byte[]> frames = prepare.get();
            if (frames == null) return null;
            remaining = operations.remainingTimeout(caller.id());
            if (remaining.isZero() || !operations.isPending(caller.id())) return null;
            // §5: after this binding operation starts, route changes cannot resubmit it.
            return Objects.requireNonNull(
                    submit.apply(
                            frames,
                            remaining,
                            admission,
                            (reply, failure) ->
                                    terminal.accept(
                                            reply,
                                            failure == null ? null : terminalFailure(failure)),
                            progress),
                    "binding request");
        } catch (RuntimeException failure) {
            terminal.accept(null, terminalFailure(failure));
            return SUBMITTING;
        }
    }

    private static Throwable terminalFailure(Throwable failure) {
        Throwable cause = ZLinkJavaRawMeshNode.unwrap(failure);
        // Preserve the existing lifecycle caller's typed initial rejection.
        return cause instanceof ZlinkSubmitException submit
                        && submit.getResult() == SubmitResult.NOT_ADMITTED
                ? cause
                : requestFailure(cause);
    }

    private void endTarget() {
        terminal.accept(
                null,
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "remote request connection admission ended"));
    }

    private void release() {
        if (pending == null) {
            var cleanup = abandon;
            abandon = () -> {};
            cleanup.run();
        }
        if (observation != null) {
            try {
                observation.close();
            } catch (Exception failure) {
                LOGGER.log(Level.WARNING, "durable request observer cleanup failed", failure);
            }
            observation = null;
        }
    }
}
