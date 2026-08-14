package systems.zlink.framework.runtime.spots;
import java.util.Map;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

/**
 * Executes one Entry Spot Actor relocation through the same fenced control
 * channel used by User Spot aggregates.
 */
final class ZLinkStandaloneActorRelocationScheduler {
    CompletionStage<Void> executeRemote(
        ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource source,
        ZLinkRelocationTransitionClient client,
        Duration timeout,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(client, "client");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(cancellation, "cancellation");
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "remote Actor relocation timeout must be positive"));
        }

        var request = source.stageRequest();
        CompletionStage<Void> operation = client.stage(
                request.targetNodeRid(), request, timeout)
            .thenCompose(ignored -> source.relayCapturedIngress(
                client, timeout))
            .thenCompose(ignored -> client.publish(
                    request.targetNodeRid(), request.fence(), timeout)
                .thenRun(source::completeSourceQueueCommit)
                .thenCompose(cleaned -> source.cleanupLocal())
                .thenCompose(cleaned -> source.discardInitialAfterCommit())
                .thenRun(() -> {
                    systems.zlink.framework.runtime.internal.metrics
                        .ZLinkRuntimeMetrics.increment(
                            "zlink.drain.actors.handed_off",
                            Map.of());
                }));
        return operation.exceptionallyCompose(failure -> {
            Throwable original = unwrap(failure);
            if (source.relayBoundaryCommitted()) {
                return CompletableFuture.failedFuture(original);
            }
            return client.abort(
                    request.targetNodeRid(), request.fence(), timeout)
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(abortFailure -> abortFailure == null
                    ? source.abort()
                    .handle((ignored, sourceFailure) -> {
                        if (sourceFailure != null) {
                            original.addSuppressed(unwrap(sourceFailure));
                        }
                        throw new CompletionException(original);
                    })
                    : CompletableFuture.failedFuture(
                        withSuppressed(original, unwrap(abortFailure))));
        });
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static CompletionException withSuppressed(
        Throwable original,
        Throwable suppressed) {
        original.addSuppressed(suppressed);
        return new CompletionException(original);
    }
}
