package systems.zlink.stream.connector;

import java.io.IOException;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeoutException;

public final class ZLinkStreamAssert {
    private ZLinkStreamAssert() {
    }

    public static void ensure(boolean condition, String message) {
        if (message == null || message.isBlank()) {
            throw new IllegalArgumentException("message is required");
        }
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    public static ZLinkStreamError expectFailure(
        ThrowingRunnable action,
        String errorKind) {
        Objects.requireNonNull(action, "action");
        Throwable failure;
        try {
            action.run();
        } catch (Throwable error) {
            failure = unwrap(error);
            return requireKind(classify(failure), errorKind);
        }
        throw new IllegalStateException("Expected action to fail.");
    }

    private static ZLinkStreamError requireKind(
        ZLinkStreamError streamError,
        String errorKind) {
        if (errorKind != null && !streamError.code().name().equals(errorKind)) {
            throw new IllegalStateException(
                "Expected failure kind '" + errorKind + "', got '"
                    + streamError.code().name() + "'.",
                streamError.exception());
        }
        return streamError;
    }

    public static void expectTimeout(ThrowingRunnable action) {
        ZLinkStreamError error = expectFailure(action, null);
        if (error.code() != ZLinkStreamErrorCode.REQUEST_TIMEOUT
            && error.code() != ZLinkStreamErrorCode.CONNECT_TIMEOUT) {
            rethrow(error.exception());
        }
    }

    private static ZLinkStreamError classify(Throwable failure) {
        ZLinkStreamErrorCode code;
        if (failure instanceof TimeoutException) {
            code = failure.getMessage() != null && failure.getMessage().startsWith("connect timed out")
                ? ZLinkStreamErrorCode.CONNECT_TIMEOUT
                : ZLinkStreamErrorCode.REQUEST_TIMEOUT;
        } else if (failure instanceof IllegalArgumentException) {
            code = ZLinkStreamErrorCode.VALIDATION_FAILED;
        } else if (failure instanceof IOException) {
            code = ZLinkStreamErrorCode.DISCONNECTED;
        } else {
            code = ZLinkStreamErrorCode.REMOTE_ERROR;
        }
        String message = failure.getMessage();
        if (message == null || message.isBlank()) {
            message = failure.getClass().getSimpleName();
        }
        return new ZLinkStreamError(code, message, failure);
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static void rethrow(Throwable error) {
        if (error instanceof RuntimeException runtime) {
            throw runtime;
        }
        if (error instanceof Error fatal) {
            throw fatal;
        }
        throw new CompletionException(error);
    }

    @FunctionalInterface
    public interface ThrowingRunnable {
        void run() throws Exception;
    }
}
