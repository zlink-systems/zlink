package systems.zlink.framework.runtime.internal.calls;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

/** Shared one-way admission and error mapping for all runtime families. */
public final class ZLinkOneWayCalls {
    public static final int SUBMITTED = 0;
    public static final int BACKPRESSURED = 1;
    public static final int TIMED_OUT = 2;
    public static final int ROUTE_NOT_CONNECTED = 3;
    public static final int TARGET_NOT_FOUND = 4;
    public static final int SHUTDOWN = 5;
    // CompletableFuture.completedStage returns a minimal stage: callers receive
    // an independent CompletableFuture from toCompletableFuture(), so the
    // shared already-completed admission still satisfies stage isolation.
    private static final CompletionStage<Void> IMMEDIATE_ADMISSION =
            CompletableFuture.completedStage(null);

    private ZLinkOneWayCalls() {}

    public static <T> CompletionStage<T> beginOneWay(AtomicBoolean submitted) {
        if (submitted.compareAndSet(false, true)) {
            return null;
        }
        return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                        "call has already been submitted"));
    }

    public static CompletionStage<Void> oneWayStatus(int status) {
        RuntimeException failure = failureForStatus(status);
        return failure == null
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(failure);
    }

    public static RuntimeException failureForStatus(int status) {
        return switch (status) {
            case SUBMITTED -> null;
            case TIMED_OUT ->
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "one-way submission did not obtain queue capacity before the send"
                                    + " deadline");
            case BACKPRESSURED ->
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "submission has no capacity waiter");
            case ROUTE_NOT_CONNECTED ->
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE, "one-way route is not connected");
            //  Framework-generated admission terminal: the marker keeps
            //  NotFound usable as the stale-route control signal now that
            //  stale detection requires kind + framework origin.
            case TARGET_NOT_FOUND ->
                    ZLinkFrameworkErrorOrigin.framework(
                            ZLinkFrameworkErrorKind.NOT_FOUND, "one-way target was not found");
            case SHUTDOWN ->
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                            "framework runtime is shutting down");
            default ->
                    throw new IllegalArgumentException(
                            "unknown one-way admission status: " + status);
        };
    }

    /**
     * Returns the binding's successful DONT_WAIT admission marker.
     *
     * <p>Only this marker means that admission is already terminal. A generic completed stage can
     * still represent a mapped failure and must retain the normal one-way adapter.
     */
    public static CompletionStage<Void> immediateAdmission() {
        return IMMEDIATE_ADMISSION;
    }

    /** True when the binding accepted this send before returning to Framework. */
    public static boolean isImmediateAdmission(CompletionStage<Void> submission) {
        return submission == IMMEDIATE_ADMISSION;
    }

    public static CompletionStage<Void> adaptOneWay(CompletionStage<Void> submission) {
        return adaptOneWay(submission, false);
    }

    public static CompletionStage<Void> adaptOneWay(
            CompletionStage<Void> submission, boolean initialSubmission) {
        if (isImmediateAdmission(submission)) {
            return submission;
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        submission.whenComplete(
                (ignored, error) -> {
                    if (error == null) {
                        result.complete(null);
                        return;
                    }
                    Throwable cause = unwrap(error);
                    if (cause instanceof ZlinkSubmitException submit) {
                        RequestResult terminal =
                                submit.getResult() == SubmitResult.BACKPRESSURED
                                                || (!initialSubmission
                                                        && submit.getResult()
                                                                == SubmitResult.NOT_FOUND)
                                        ? RequestResult.NOT_CONNECTED
                                        : toRequestResult(submit.getResult(), initialSubmission);
                        result.completeExceptionally(
                                ZLinkFrameworkErrorOrigin.framework(
                                        ZLinkBackendRequestResult.fromWireTerminal(terminal.value())
                                                .toFrameworkErrorKind(),
                                        submit.getMessage(),
                                        submit));
                        return;
                    }
                    result.completeExceptionally(cause);
                });
        return result;
    }

    /** Owns the typed submit projection; capacity meaning depends on admission phase. */
    public static RequestResult toRequestResult(SubmitResult result, boolean initialSubmission) {
        return switch (result) {
            case OK -> RequestResult.OK;
            case BACKPRESSURED ->
                    initialSubmission ? RequestResult.BACKPRESSURED : RequestResult.TIMED_OUT;
            case NOT_CONNECTED -> RequestResult.NOT_CONNECTED;
            case NOT_FOUND -> RequestResult.NOT_FOUND;
            case NOT_ADMITTED -> RequestResult.REJECTED;
            case TERMINATED -> RequestResult.TERMINATED;
            case INVALID_STATE -> RequestResult.INVALID_STATE;
            case INVALID_ARGUMENT, INVALID_HANDLE, THREAD_VIOLATION ->
                    RequestResult.INVALID_ARGUMENT;
            case NOT_SUPPORTED -> RequestResult.NOT_SUPPORTED;
            default -> RequestResult.INTERNAL_ERROR;
        };
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
