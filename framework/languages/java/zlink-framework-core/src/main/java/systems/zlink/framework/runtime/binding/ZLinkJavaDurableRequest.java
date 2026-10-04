package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BiFunction;
import java.util.function.BooleanSupplier;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Owns one durable lifecycle request and observes its logical route owner. */
final class ZLinkJavaDurableRequest {
    private static final Logger LOGGER = Logger.getLogger(ZLinkJavaDurableRequest.class.getName());
    private final Supplier<List<byte[]>> prepare;
    private final BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit;
    private final BooleanSupplier targetLifecycleEnded;
    private final CompletableFuture<List<byte[]>> completion = new CompletableFuture<>();
    private final LongSupplier nanoTime;
    private final long started;
    private final long timeoutNanos;
    private final Function<Runnable, CompletionStage<Void>> owner;
    private List<byte[]> frames;
    private boolean admitted;
    private Throwable lastFailure;
    private CompletionStage<List<byte[]>> pending;
    private CompletableFuture<Void> routeChange;

    private ZLinkJavaDurableRequest(
            Supplier<List<byte[]>> prepare,
            BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit,
            BooleanSupplier targetLifecycleEnded,
            Duration timeout,
            Function<Runnable, CompletionStage<Void>> owner,
            LongSupplier nanoTime) {
        this.nanoTime = nanoTime;
        this.started = nanoTime.getAsLong();
        this.prepare = prepare;
        this.submit = submit;
        this.targetLifecycleEnded = targetLifecycleEnded;
        this.timeoutNanos = timeout.toNanos();
        this.owner = owner;
    }

    static CompletionStage<List<byte[]>> request(
            Supplier<List<byte[]>> prepare,
            BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit,
            BooleanSupplier targetLifecycleEnded,
            Function<Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout) {
        return request(
                prepare, submit, targetLifecycleEnded, observe, owner, timeout, System::nanoTime);
    }

    static CompletionStage<List<byte[]>> request(
            Supplier<List<byte[]>> prepare,
            BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit,
            BooleanSupplier targetLifecycleEnded,
            Function<Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            LongSupplier nanoTime) {
        var request =
                new ZLinkJavaDurableRequest(
                        prepare, submit, targetLifecycleEnded, timeout, owner, nanoTime);
        AutoCloseable registration;
        try {
            registration = observe.apply(() -> request.post(request::sourceChanged));
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        var deadline =
                ZLinkProcessExecutionLanes.deadlines()
                        .schedule(
                                () -> {
                                    // A started attempt already owns the binding's remaining
                                    // deadline.
                                    request.post(
                                            () -> {
                                                if (request.pending == null) request.exhaust();
                                            });
                                },
                                Math.max(0, request.remaining()),
                                TimeUnit.NANOSECONDS);
        request.completion.whenComplete(
                (reply, failure) -> {
                    deadline.cancel(false);
                    try {
                        registration.close();
                    } catch (Exception cleanupFailure) {
                        LOGGER.log(
                                Level.WARNING,
                                "durable request observer cleanup failed",
                                cleanupFailure);
                    }
                    if (request.completion.isCancelled())
                        request.post(
                                () -> {
                                    if (request.pending != null)
                                        request.pending.toCompletableFuture().cancel(false);
                                });
                });
        return request.completion;
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

    private void sourceChanged() {
        if (completion.isDone()) return;
        if (targetLifecycleEnded.getAsBoolean()) {
            endTarget();
        } else if (routeChange == null) {
            attempt();
        } else {
            routeChange.complete(null);
        }
    }

    private void attempt() {
        if (completion.isDone()) return;
        if (targetLifecycleEnded.getAsBoolean()) {
            endTarget();
            return;
        }
        if (pending != null) return;
        if (remaining() <= 0) {
            exhaust();
            return;
        }
        routeChange = new CompletableFuture<>();
        try {
            if (frames == null) {
                frames = prepare.get();
                if (frames == null) {
                    routeChange.thenRun(this::attempt);
                    return;
                }
            }
            long remaining = remaining();
            if (remaining <= 0) {
                exhaust();
                return;
            }
            pending = submit.apply(frames, Duration.ofNanos(remaining));
            pending.whenComplete((reply, failure) -> post(() -> settle(reply, failure, false)));
        } catch (RuntimeException failure) {
            settle(null, failure, true);
        }
    }

    private void settle(List<byte[]> reply, Throwable failure, boolean initialSubmission) {
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
        if (targetLifecycleEnded.getAsBoolean()) {
            endTarget();
            return;
        }
        if (cause instanceof ZlinkRequestException request) {
            admitted = true;
            if (request.getResult() == RequestResult.NOT_CONNECTED) {
                waitForRouteChange();
                return;
            }
            if (request.getResult() == RequestResult.TIMED_OUT) {
                CompletableFuture.runAsync(() -> post(this::attempt));
                return;
            }
        } else if (cause instanceof ZlinkSubmitException initial) {
            if (initial.getResult() == SubmitResult.NOT_CONNECTED
                    || initial.getResult() == SubmitResult.NOT_FOUND) {
                waitForRouteChange();
                return;
            }
            if (initial.getResult() == SubmitResult.NOT_ADMITTED) {
                completion.completeExceptionally(cause);
                return;
            }
        }
        RequestResult terminal = ZLinkJavaRawMeshNode.requestResult(cause, initialSubmission);
        completion.completeExceptionally(
                terminal == null
                        ? cause
                        : new ZLinkFrameworkException(
                                ZLinkJavaRawMeshNode.backendResult(terminal).toFrameworkErrorKind(),
                                "durable request submission failed",
                                cause));
    }

    private void waitForRouteChange() {
        if (remaining() <= 0) exhaust();
        else routeChange.thenRun(this::attempt);
    }

    private void endTarget() {
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
                        admitted
                                ? ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        admitted
                                ? "durable request reply was not received before its deadline"
                                : "durable request was not admitted before its deadline",
                        lastFailure));
    }
}
