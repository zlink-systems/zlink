package systems.zlink.framework.runtime.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkOneWayAdmission {
    private ZLinkOneWayAdmission() {
    }

    static <T> CompletionStage<T> begin(
        java.util.concurrent.atomic.AtomicBoolean submitted) {
        if (submitted.compareAndSet(false, true)) {
            return null;
        }
        return CompletableFuture.failedFuture(new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            "call has already been submitted"));
    }

    static CompletionStage<Void> fromVoidStage(
        CompletionStage<Void> submission) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        submission.whenComplete((ignored, error) -> {
            if (error == null) {
                result.complete(null);
                return;
            }
            Throwable cause = unwrap(error);
            RuntimeException mapped = fromFailure(cause);
            if (mapped != null) {
                result.completeExceptionally(mapped);
            } else {
                result.completeExceptionally(cause);
            }
        });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled()) {
                submission.toCompletableFuture().cancel(false);
            }
        });
        return result;
    }

    static RuntimeException result(ZLinkOneWayAdmissionStatus status) {
        return switch (status) {
            case SUBMITTED -> null;
            case TIMED_OUT, BACKPRESSURED -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                "one-way submission did not obtain queue capacity before the send deadline");
            case ROUTE_NOT_CONNECTED -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "one-way route is not connected");
            case TARGET_NOT_FOUND -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "one-way target was not found");
            case SHUTDOWN -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                "framework runtime is shutting down");
        };
    }

    static CompletionStage<Void> fromStatus(ZLinkOneWayAdmissionStatus status) {
        RuntimeException failure = result(status);
        return failure == null
            ? CompletableFuture.completedFuture(null)
            : CompletableFuture.failedFuture(failure);
    }

    static RuntimeException fromFailure(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof TimeoutException) {
            return result(ZLinkOneWayAdmissionStatus.TIMED_OUT);
        }
        if (!(cause instanceof ZlinkSubmitException submit)) {
            return null;
        }
        SubmitResult nativeResult = submit.getResult();
        return switch (nativeResult) {
            case BACKPRESSURED, NOT_ADMITTED -> result(ZLinkOneWayAdmissionStatus.BACKPRESSURED);
            case NOT_CONNECTED -> result(ZLinkOneWayAdmissionStatus.ROUTE_NOT_CONNECTED);
            case NOT_FOUND -> result(ZLinkOneWayAdmissionStatus.TARGET_NOT_FOUND);
            case TERMINATED -> result(ZLinkOneWayAdmissionStatus.SHUTDOWN);
            default -> null;
        };
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
                || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
