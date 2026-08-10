package systems.zlink.framework.runtime.spots;
import java.util.Map;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
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
        var committed = new AtomicBoolean();
        CompletionStage<Void> operation = client.stage(
                request.targetNodeRid(), request, timeout)
            .thenCompose(ignored -> source.freezeAndPrepare(cancellation))
            .thenCompose(ignored -> source.commitAuthority(cancellation))
            .thenCompose(published -> {
                committed.set(true);
                return source.commitSourceQueue(published, cancellation)
                    .thenCompose(activated -> client.publish(
                            request.targetNodeRid(), request.fence(), timeout)
                        .thenRun(source::completeSourceQueueCommit)
                        .thenCompose(ignored -> source.cleanupLocal())
                        .thenCompose(ignored -> source.completeSourceCleanup(
                            activated, cancellation))
                        .thenCompose(completed -> client.finalizeAfterCompletion(
                            request.targetNodeRid(), request.fence(), timeout))
                        .thenCompose(ignored ->
                            source.discardInitialAfterCommit())
                        .thenRun(() -> {
                            source.releasePermitAfterCompletion();
                            systems.zlink.framework.runtime.internal.metrics
                                .ZLinkRuntimeMetrics.increment(
                                    "zlink.drain.actors.handed_off",
                                    Map.of());
                        }));
            });
        return operation.exceptionallyCompose(failure -> {
            Throwable original = unwrap(failure);
            if (committed.get()) {
                return CompletableFuture.failedFuture(original);
            }
            return client.abort(
                    request.targetNodeRid(), request.fence(), timeout)
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(abortFailure -> source.abort()
                    .handle((ignored, sourceFailure) -> {
                        if (abortFailure != null) {
                            original.addSuppressed(unwrap(abortFailure));
                        }
                        if (sourceFailure != null) {
                            original.addSuppressed(unwrap(sourceFailure));
                        }
                        throw new CompletionException(original);
                    }));
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
}
