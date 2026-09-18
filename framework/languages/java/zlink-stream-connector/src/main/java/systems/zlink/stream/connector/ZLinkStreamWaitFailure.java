package systems.zlink.stream.connector;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;

/**
 * Translates the failures of the observation surfaces
 * ({@code waitFor}, {@code expectNone}, {@code waitForSequence}) into the one
 * code common connector spec 32 10.1 assigns them: every failure of these
 * surfaces is {@code VALIDATION_FAILED}, delivered per 9.2 in an exception
 * that carries the code.
 */
final class ZLinkStreamWaitFailure {
    private ZLinkStreamWaitFailure() {
    }

    static <T> CompletableFuture<T> asValidationFailure(
        CompletableFuture<T> source,
        String message) {
        CompletableFuture<T> result = new CompletableFuture<>();
        source.whenComplete((value, error) -> {
            if (error == null) {
                result.complete(value);
                return;
            }
            result.completeExceptionally(translate(error, message));
        });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled()) {
                source.cancel(false);
            }
        });
        return result;
    }

    private static Throwable translate(Throwable error, String message) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZLinkStreamException alreadyCoded) {
            return alreadyCoded;
        }
        if (cause instanceof CancellationException cancelled) {
            //  Cancellation is the caller's own decision, not a violation of
            //  the observation condition.
            return cancelled;
        }
        return ZLinkStreamException.validationFailed(message, cause);
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
            || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
