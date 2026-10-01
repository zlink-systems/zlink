package systems.zlink.stream.connector;

import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;

public final class ZLinkStreamAssert {
    private ZLinkStreamAssert() {}

    public static void ensure(boolean condition, String message) {
        if (message == null || message.isBlank()) {
            throw new IllegalArgumentException("message is required");
        }
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    public static ZLinkStreamError expectFailure(ThrowingRunnable action, String errorKind) {
        Objects.requireNonNull(action, "action");
        Throwable failure;
        try {
            action.run();
        } catch (Throwable error) {
            failure = unwrap(error);
            return requireKind(classify(failure, error), errorKind);
        }
        throw new IllegalStateException("Expected action to fail.");
    }

    private static ZLinkStreamError requireKind(ZLinkStreamError streamError, String errorKind) {
        if (errorKind != null && !streamError.code().name().equals(errorKind)) {
            throw new IllegalStateException(
                    "Expected failure kind '"
                            + errorKind
                            + "', got '"
                            + streamError.code().name()
                            + "'.",
                    streamError.exception());
        }
        return streamError;
    }

    public static void expectTimeout(ThrowingRunnable action) {
        Objects.requireNonNull(action, "action");
        try {
            action.run();
        } catch (Throwable original) {
            ZLinkStreamError error = classify(unwrap(original), original);
            if (error.code() == ZLinkStreamErrorCode.REQUEST_TIMEOUT
                    || error.code() == ZLinkStreamErrorCode.CONNECT_TIMEOUT) {
                return;
            }
            rethrow(original);
        }
        throw new IllegalStateException("Expected action to time out.");
    }

    private static ZLinkStreamError classify(Throwable failure, Throwable original) {
        //  Spec 32 9.2: when the connector itself reports a failure it
        //  carries the code, so read it instead of guessing from the type.
        if (failure instanceof ZLinkStreamException coded) {
            return coded.error();
        }
        return rethrow(original);
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException || current instanceof ExecutionException)
                && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    @SuppressWarnings("unchecked")
    private static <R, T extends Throwable> R rethrow(Throwable error) throws T {
        throw (T) error;
    }

    @FunctionalInterface
    public interface ThrowingRunnable {
        void run() throws Exception;
    }
}
