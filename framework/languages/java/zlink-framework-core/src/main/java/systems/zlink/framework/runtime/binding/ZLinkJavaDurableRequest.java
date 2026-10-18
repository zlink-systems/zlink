package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerRouteObservation;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Owns one durable lifecycle request and observes its logical route owner. */
public final class ZLinkJavaDurableRequest {
    private static final Logger LOGGER = Logger.getLogger(ZLinkJavaDurableRequest.class.getName());
    private final Supplier<List<byte[]>> prepare;

    @FunctionalInterface
    public interface Submit {
        CompletionStage<List<byte[]>> apply(
                List<byte[]> frames, Duration remaining, CompletableFuture<Void> admission);
    }

    private final Submit submit;
    private final Supplier<PeerRouteObservation> route;
    private final CompletableFuture<List<byte[]>> completion = new CompletableFuture<>();
    private final CompletableFuture<List<byte[]>> caller = new CompletableFuture<>();
    private final LongSupplier nanoTime;
    private final long started;
    private final long timeoutNanos;
    private final Function<Runnable, CompletionStage<Void>> owner;
    private List<byte[]> frames;
    private final CompletableFuture<Void> admission = new CompletableFuture<>();
    private Throwable lastFailure;
    private CompletionStage<List<byte[]>> pending;
    private long submittedRoute;

    private ZLinkJavaDurableRequest(
            Supplier<List<byte[]>> prepare,
            Submit submit,
            Supplier<PeerRouteObservation> route,
            Duration timeout,
            Function<Runnable, CompletionStage<Void>> owner,
            LongSupplier nanoTime) {
        this.nanoTime = nanoTime;
        this.started = nanoTime.getAsLong();
        this.prepare = prepare;
        this.submit = submit;
        this.route = route;
        this.timeoutNanos = timeout.toNanos();
        this.owner = owner;
    }

    public static CompletionStage<List<byte[]>> request(
            Supplier<List<byte[]>> prepare,
            Submit submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout) {
        return request(prepare, submit, route, observe, owner, timeout, System::nanoTime);
    }

    public static CompletionStage<List<byte[]>> request(
            Supplier<List<byte[]>> prepare,
            Submit submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            LongSupplier nanoTime) {
        var request = new ZLinkJavaDurableRequest(prepare, submit, route, timeout, owner, nanoTime);
        AutoCloseable registration;
        try {
            registration =
                    observe.apply(
                            () -> request.post(request::sourceChanged),
                            () -> request.post(request::endTarget));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        var deadline =
                ZLinkProcessExecutionLanes.deadlines()
                        .schedule(
                                () -> {
                                    request.post(
                                            () -> {
                                                request.settleReceived();
                                                if (!request.completion.isDone()) request.exhaust();
                                            });
                                },
                                Math.max(0, request.remaining()),
                                TimeUnit.NANOSECONDS);
        request.completion.whenComplete(
                (reply, failure) -> {
                    try {
                        deadline.cancel(false);
                        try {
                            registration.close();
                        } catch (Exception cleanupFailure) {
                            LOGGER.log(
                                    Level.WARNING,
                                    "durable request observer cleanup failed",
                                    cleanupFailure);
                        }
                    } finally {
                        ZLinkStateLane.publishOutsideLane(
                                request.completion,
                                (value, terminalFailure) -> {
                                    if (terminalFailure == null) request.caller.complete(value);
                                    else request.caller.completeExceptionally(terminalFailure);
                                });
                    }
                });
        ZLinkCompletionBridge.forwardCancellation(
                request.completion,
                () ->
                        request.post(
                                () -> {
                                    if (request.pending != null)
                                        request.pending.toCompletableFuture().cancel(false);
                                }));
        ZLinkCompletionBridge.forwardCancellation(request.caller, request.completion);
        return request.caller;
    }

    private void post(Runnable action) {
        try {
            owner.apply(action)
                    .whenComplete(
                            (unused, failure) -> {
                                if (failure != null && !completion.completeExceptionally(failure)) {
                                    LOGGER.log(
                                            Level.WARNING,
                                            "durable request owner turn failed after terminal",
                                            failure);
                                }
                            });
        } catch (RuntimeException failure) {
            completion.completeExceptionally(failure);
        }
    }

    private long remaining() {
        return timeoutNanos - (nanoTime.getAsLong() - started);
    }

    private boolean settleReceived() {
        if (pending == null || !pending.toCompletableFuture().isDone()) return false;
        var received = pending;
        // A received terminal wins before a later route, removal, or deadline turn.
        received.whenComplete((reply, failure) -> settle(received, reply, failure));
        return true;
    }

    private void sourceChanged() {
        if (completion.isDone()) return;
        if (pending != null) {
            var previous = pending;
            if (settleReceived()) return;
            if (route.get().selectedGeneration() == submittedRoute) return;
            pending = null;
            previous.toCompletableFuture().cancel(false);
        }
        attempt();
    }

    private void attempt() {
        if (completion.isDone()) return;
        if (pending != null) return;
        if (remaining() <= 0) {
            exhaust();
            return;
        }
        PeerRouteObservation currentRoute = route.get();
        if (!currentRoute.ready()) return;
        try {
            if (frames == null) {
                frames = prepare.get();
                if (frames == null) {
                    return;
                }
            }
            long remaining = remaining();
            if (remaining <= 0) {
                exhaust();
                return;
            }
            submittedRoute = currentRoute.selectedGeneration();
            pending = submit.apply(frames, Duration.ofNanos(remaining), admission);
            var submitted = pending;
            submitted.whenComplete(
                    (reply, failure) -> post(() -> settle(submitted, reply, failure)));
        } catch (RuntimeException failure) {
            settle(null, null, failure);
        }
    }

    private void settle(
            CompletionStage<List<byte[]>> submitted, List<byte[]> reply, Throwable failure) {
        if (submitted != pending) return;
        pending = null;
        if (completion.isDone()) return;
        if (failure == null) {
            completion.complete(reply);
            return;
        }
        Throwable cause = failure;
        while ((cause instanceof CompletionException || cause instanceof ExecutionException)
                && cause.getCause() != null) cause = cause.getCause();
        lastFailure = cause;
        if (cause instanceof ZlinkRequestException request) {
            admission.complete(null);
            if (request.getResult() == RequestResult.NOT_CONNECTED) {
                replayWhenReady();
                return;
            }
            if (request.getResult() == RequestResult.TIMED_OUT) {
                replayWhenReady();
                return;
            }
        } else if (cause instanceof ZlinkSubmitException initial) {
            if (initial.getResult() == SubmitResult.NOT_CONNECTED
                    || initial.getResult() == SubmitResult.NOT_FOUND) {
                replayWhenReady();
                return;
            }
            if (initial.getResult() == SubmitResult.NOT_ADMITTED) {
                completion.completeExceptionally(cause);
                return;
            }
        }
        completion.completeExceptionally(ZLinkJavaRawMeshNode.requestFailure(cause));
    }

    private void replayWhenReady() {
        if (remaining() <= 0) exhaust();
        else if (route.get().ready()) {
            // A replay gets another owner turn. Immediate native rejections cannot
            // recurse in the current turn; no retry timer or additional queue is needed.
            CompletableFuture.runAsync(() -> post(this::attempt));
        }
    }

    private void endTarget() {
        settleReceived();
        if (completion.isDone()) return;
        completion.completeExceptionally(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "durable request target lifecycle ended",
                        lastFailure));
        if (pending != null) pending.toCompletableFuture().cancel(false);
    }

    private void exhaust() {
        completion.completeExceptionally(
                new ZLinkFrameworkException(
                        admission.isDone() && !admission.isCompletedExceptionally()
                                ? ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        admission.isDone() && !admission.isCompletedExceptionally()
                                ? "durable request reply was not received before its deadline"
                                : "durable request was not admitted before its deadline",
                        lastFailure));
        if (pending != null) pending.toCompletableFuture().cancel(false);
    }
}
