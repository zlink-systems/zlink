package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;

public final class ZLinkObjectCreationDeadline {
    private ZLinkObjectCreationDeadline() {}

    public static <T> CompletionStage<T> classify(
            CompletionStage<T> creation, long deadlineUnixMs) {
        return creation.exceptionallyCompose(
                error -> {
                    Throwable cause = error;
                    while (cause instanceof CompletionException && cause.getCause() != null) {
                        cause = cause.getCause();
                    }
                    if ((cause instanceof CancellationException
                                    || cause instanceof TimeoutException)
                            && System.currentTimeMillis() >= deadlineUnixMs) {
                        return CompletableFuture.failedFuture(exceeded(cause));
                    }
                    return CompletableFuture.failedFuture(cause);
                });
    }

    public static ZLinkFrameworkException exceeded(Throwable cause) {
        return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                "Object creation deadline expired",
                cause);
    }
}
