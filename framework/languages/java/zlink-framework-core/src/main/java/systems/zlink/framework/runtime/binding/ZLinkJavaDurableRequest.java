package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BiFunction;
import java.util.function.Supplier;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Owns the encoded identity and typed admission history of one durable request. */
final class ZLinkJavaDurableRequest {
    private final Supplier<List<byte[]>> prepare;
    private final BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit;
    private final CompletableFuture<List<byte[]>> completion = new CompletableFuture<>();
    private final long deadline;
    private List<byte[]> frames;
    private boolean admitted;
    private Throwable lastFailure;

    private ZLinkJavaDurableRequest(
        Supplier<List<byte[]>> prepare,
        BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit,
        Duration timeout) {
        this.prepare = prepare;
        this.submit = submit;
        this.deadline = System.nanoTime() + timeout.toNanos();
    }

    static CompletionStage<List<byte[]>> request(
        Supplier<List<byte[]>> prepare,
        BiFunction<List<byte[]>, Duration, CompletionStage<List<byte[]>>> submit,
        Duration timeout) {
        var request = new ZLinkJavaDurableRequest(prepare, submit, timeout);
        request.attempt();
        return request.completion;
    }

    private void attempt() {
        if (completion.isDone()) {
            return;
        }
        if (deadline - System.nanoTime() <= 0) {
            exhaust();
            return;
        }
        try {
            if (frames == null) {
                // A missing logical route has not reached binding admission.
                // Freeze the first complete header, including its correlation.
                frames = prepare.get();
                if (frames == null) {
                    retry();
                    return;
                }
            }
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                exhaust();
                return;
            }
            submit.apply(frames, Duration.ofNanos(remaining))
                .whenComplete(this::settle);
        } catch (RuntimeException failure) {
            settle(null, failure);
        }
    }

    private void settle(List<byte[]> reply, Throwable failure) {
        if (completion.isDone()) {
            return;
        }
        if (failure == null) {
            // Decode outside replay: a received terminal (including rejection)
            // or a malformed reply must never cause another execution attempt.
            completion.complete(reply);
            return;
        }
        Throwable cause = failure;
        while ((cause instanceof CompletionException
            || cause instanceof ExecutionException) && cause.getCause() != null) {
            cause = cause.getCause();
        }
        lastFailure = cause;
        if (cause instanceof ZlinkRequestException request) {
            admitted = true;
            if (request.getResult() == RequestResult.NOT_CONNECTED
                || request.getResult() == RequestResult.TIMED_OUT) {
                retry();
                return;
            }
        } else if (cause instanceof ZlinkSubmitException initial) {
            SubmitResult result = initial.getResult();
            if (result == SubmitResult.NOT_CONNECTED
                || result == SubmitResult.NOT_FOUND
                || result == SubmitResult.BACKPRESSURED
                || result == SubmitResult.NOT_ADMITTED) {
                retry();
                return;
            }
        }
        completion.completeExceptionally(cause);
    }

    private void retry() {
        long remaining = deadline - System.nanoTime();
        if (remaining <= 0) {
            exhaust();
            return;
        }
        ZLinkProcessExecutionLanes.deadlines().schedule(
            this::attempt,
            Math.min(remaining, TimeUnit.MILLISECONDS.toNanos(10)),
            TimeUnit.NANOSECONDS);
    }

    private void exhaust() {
        completion.completeExceptionally(new ZLinkFrameworkException(
            admitted ? ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                : ZLinkFrameworkErrorKind.UNAVAILABLE,
            admitted ? "durable request reply was not received before its deadline"
                : "durable request was not admitted before its deadline",
            lastFailure));
    }
}
