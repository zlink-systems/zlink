package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.SpotHandle;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;

public interface SpotTransportAddressResolver {
    default CompletionStage<Optional<SpotTransportAddress>> resolve(SpotHandle handle) {
        return resolve(handle.spotId());
    }

    CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId);

    static CompletionStage<SpotTransportAddress> resolveForCall(
            SpotTransportAddressResolver resolver, String spotId, boolean instanceIntent) {
        if (resolver == null) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException("SpotHandle resolver is not configured"));
        }
        return resolveForCall(resolver.resolve(spotId), instanceIntent);
    }

    static CompletionStage<SpotTransportAddress> resolveForCall(
            CompletionStage<Optional<SpotTransportAddress>> resolution, boolean instanceIntent) {
        return resolution.handle(
                (address, failure) -> {
                    if (failure != null) {
                        Throwable cause = failure;
                        while ((cause instanceof CompletionException
                                        || cause instanceof ExecutionException)
                                && cause.getCause() != null) cause = cause.getCause();
                        if (instanceIntent
                                && (isStaleRoute(cause)
                                        || cause instanceof ZLinkFrameworkException error
                                                && error.kind()
                                                        == ZLinkFrameworkErrorKind.UNAVAILABLE))
                            return null;
                        throw new CompletionException(cause);
                    }
                    if (address.isPresent()) return address.get();
                    if (instanceIntent) return null;
                    throw ZLinkFrameworkErrorOrigin.framework(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            "SpotHandle route is stale or unavailable");
                });
    }

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
