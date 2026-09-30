package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.SpotHandle;

import java.util.Optional;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;

public interface SpotTransportAddressResolver {
    default CompletionStage<Optional<SpotTransportAddress>> resolve(SpotHandle handle) {
        return resolve(handle.spotId());
    }

    CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId);

    /**
     * Removes a cached positive route after the target reports that the resolved owner no longer
     * accepts it. Custom resolvers may keep this as a no-op when they do not cache routes.
     */
    default void invalidate(String spotId) {}

    /** Removes a stale positive route for the next call while retaining this call's terminal. */
    default <T> CompletionStage<T> observeTerminal(String spotId, CompletionStage<T> stage) {
        return stage.whenComplete(
                (ignored, failure) -> {
                    if (isStaleRoute(failure)) {
                        invalidate(spotId);
                    }
                });
    }

    static boolean isStaleRoute(Throwable failure) {
        Throwable cause = failure;
        while ((cause instanceof CompletionException || cause instanceof ExecutionException)
                && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause instanceof ZLinkFrameworkException error
                && error.kind() == ZLinkFrameworkErrorKind.NOT_FOUND
                && ZLinkFrameworkErrorOrigin.isFramework(error);
    }
}
