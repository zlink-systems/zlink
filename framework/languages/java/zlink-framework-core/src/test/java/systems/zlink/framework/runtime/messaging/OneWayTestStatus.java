package systems.zlink.framework.runtime.messaging;

import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

public final class OneWayTestStatus {
    private OneWayTestStatus() {
    }

    public static Integer status(CompletionStage<Void> stage) {
        try {
            stage.toCompletableFuture().join();
            return 0;
        } catch (CompletionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof ZLinkFrameworkException frameworkError) {
                return switch (frameworkError.kind()) {
                    case DEADLINE_EXCEEDED -> 2;
                    case UNAVAILABLE -> 3;
                    case SHUTTING_DOWN -> 5;
                    default -> 4;
                };
            }
            throw error;
        }
    }
}
