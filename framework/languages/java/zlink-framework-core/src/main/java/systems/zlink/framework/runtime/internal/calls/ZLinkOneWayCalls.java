package systems.zlink.framework.runtime.internal.calls;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

/** Shared one-way admission and error mapping for all runtime families. */
public final class ZLinkOneWayCalls {
    public static final int SUBMITTED = 0;
    public static final int BACKPRESSURED = 1;
    public static final int TIMED_OUT = 2;
    public static final int ROUTE_NOT_CONNECTED = 3;
    public static final int TARGET_NOT_FOUND = 4;
    public static final int SHUTDOWN = 5;

    private ZLinkOneWayCalls() {
    }

    public static <T> CompletionStage<T> beginOneWay(AtomicBoolean submitted) {
        if (submitted.compareAndSet(false, true)) {
            return null;
        }
        return CompletableFuture.failedFuture(new ZLinkFrameworkException(
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
            case TIMED_OUT, BACKPRESSURED -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                "one-way submission did not obtain queue capacity before the send deadline");
            case ROUTE_NOT_CONNECTED -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "one-way route is not connected");
            //  Framework-generated admission terminal: the marker keeps
            //  NotFound usable as the stale-route control signal now that
            //  stale detection requires kind + framework origin.
            case TARGET_NOT_FOUND -> ZLinkFrameworkErrorOrigin.framework(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "one-way target was not found");
            case SHUTDOWN -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                "framework runtime is shutting down");
            default -> throw new IllegalArgumentException(
                "unknown one-way admission status: " + status);
        };
    }

    public static CompletionStage<Void> adaptOneWay(CompletionStage<Void> submission) {
        CompletableFuture<Void> source = submission.toCompletableFuture();
        CompletableFuture<Void> result = new CompletableFuture<>() {
            @Override
            public boolean cancel(boolean mayInterruptIfRunning) {
                boolean cancelled = super.cancel(mayInterruptIfRunning);
                if (cancelled) {
                    source.cancel(mayInterruptIfRunning);
                }
                return cancelled;
            }
        };
        submission.whenComplete((ignored, error) -> {
            if (error == null) {
                result.complete(null);
                return;
            }
            Throwable cause = unwrap(error);
            if (cause instanceof ZlinkSubmitException submit) {
                CompletionStage<Void> mapped = switch (submit.getResult()) {
                    case BACKPRESSURED -> oneWayStatus(BACKPRESSURED);
                    case NOT_ADMITTED -> oneWayStatus(
                        isRouteUnavailableErrno(submit.getNativeErrno())
                            ? ROUTE_NOT_CONNECTED
                            : BACKPRESSURED);
                    case NOT_CONNECTED -> oneWayStatus(ROUTE_NOT_CONNECTED);
                    case NOT_FOUND -> oneWayStatus(TARGET_NOT_FOUND);
                    case TERMINATED -> oneWayStatus(SHUTDOWN);
                    default -> null;
                };
                if (mapped != null) {
                    mapped.whenComplete((unused, mappedError) ->
                        result.completeExceptionally(unwrap(mappedError)));
                    return;
                }
            }
            result.completeExceptionally(cause);
        });
        return result;
    }

    private static boolean isRouteUnavailableErrno(int nativeErrno) {
        // Async binding terminals can collapse an exact-route transport loss
        // to NOT_ADMITTED. Preserve queue admission failures as
        // DeadlineExceeded, but surface portable route-loss errno values as
        // Unavailable.
        return switch (nativeErrno) {
            case 101, 107, 111, 113, 10051, 10057, 10061, 10065 -> true;
            default -> false;
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
