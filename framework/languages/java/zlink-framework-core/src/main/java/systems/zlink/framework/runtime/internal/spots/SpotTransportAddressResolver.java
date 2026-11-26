package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkRequestCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.SpotHandle;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.function.BiFunction;
import java.util.function.LongSupplier;

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

    static <T> void request(
            SpotTransportAddressResolver resolver,
            String spotId,
            boolean instanceIntent,
            Duration timeout,
            LongSupplier nanoTime,
            ZLinkServiceOperationRegistry operations,
            CompletableFuture<T> result,
            BiFunction<SpotTransportAddress, Duration, CompletionStage<T>> submit) {
        long deadline = nanoTime.getAsLong() + timeout.toNanos();
        var flow = ZLinkFlowContext.current();
        var resolution = resolveForCall(resolver, spotId, instanceIntent).toCompletableFuture();
        // An asynchronous lookup uses the existing deadline index until transport takes ownership.
        var lookup =
                resolution.isDone() || operations == null
                        ? resolution
                        : operations.submitUntil(
                                ZLinkServiceOperationIds.next(),
                                deadline,
                                () -> resolution,
                                ignored -> {});
        if (lookup != resolution) ZLinkCompletionBridge.forwardCancellation(result, lookup);
        lookup.whenComplete(
                (address, failure) -> {
                    if (result.isDone()) return;
                    if (failure != null) {
                        result.completeExceptionally(failure);
                        return;
                    }
                    try {
                        Duration remaining =
                                ZLinkRequestCalls.remainingUntil(deadline, nanoTime.getAsLong());
                        var submitted =
                                ZLinkFlowContext.call(flow, () -> submit.apply(address, remaining));
                        ZLinkCompletionBridge.forwardCancellation(result, submitted);
                        submitted.whenComplete(
                                (value, error) -> {
                                    if (error == null) result.complete(value);
                                    else result.completeExceptionally(error);
                                });
                    } catch (RuntimeException | Error error) {
                        result.completeExceptionally(error);
                    }
                });
    }

    /**
     * Removes a cached positive route after the target reports that the resolved owner no longer
     * accepts it. Custom resolvers may keep this as a no-op when they do not cache routes.
     */
    default void invalidate(String spotId) {}

    /** Removes a stale positive route for the next call while retaining this call's terminal. */
    default <T> CompletionStage<T> observeTerminal(String spotId, CompletionStage<T> stage) {
        CompletableFuture<T> caller =
                systems.zlink.framework.runtime.internal.ZLinkCompletionBridge
                        .newCancellationFuture();
        systems.zlink.framework.runtime.internal.ZLinkCompletionBridge.forwardCancellation(
                caller, stage);
        stage.whenComplete(
                        (value, failure) -> {
                            if (isStaleRoute(failure)) invalidate(spotId);
                        })
                .whenComplete(
                        (value, failure) -> {
                            if (failure == null) caller.complete(value);
                            else caller.completeExceptionally(failure);
                        });
        return caller;
    }

    static boolean isStaleRoute(Throwable failure) {
        Throwable cause = failure;
        while ((cause instanceof CompletionException || cause instanceof ExecutionException)
                && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause instanceof ZLinkFrameworkException error
                && (error.kind() == ZLinkFrameworkErrorKind.NOT_FOUND
                        || error.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE
                        || systems.zlink.framework.runtime.internal.backend
                                        .ZLinkRequestFailureMapping.causeCode(error)
                                == (int)
                                        systems.zlink.framework.runtime.protocol
                                                .ServiceWireConstants
                                                .FRAMEWORK_ERROR_SPOT_GENERATION_STALE)
                && ZLinkFrameworkErrorOrigin.isFramework(error);
    }
}
