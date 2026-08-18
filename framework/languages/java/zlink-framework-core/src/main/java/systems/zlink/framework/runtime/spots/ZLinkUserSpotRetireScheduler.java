package systems.zlink.framework.runtime.spots;
import java.util.Map;

import java.util.List;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

/**
 * Owns the source-side execution order for a User Spot relocation.
 */
final class ZLinkUserSpotRetireScheduler {
    CompletionStage<Void> executeRemote(
        RemoteRequest request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpotRetireControl.StageRequest stage =
            request.source().stageRequest();
        CompletionStage<Void> operation = request.client().stage(
                stage.targetNodeRid(),
                stage,
                request.timeout())
            .thenCompose(ignored -> request.source().relayCapturedIngress(
                request.client(), request.timeout()))
            .thenCompose(ignored -> request.client().publish(
                    stage.targetNodeRid(),
                    stage.fence(),
                    request.timeout())
                .thenRun(request.source()::completeSourceBarrierCommit)
                .thenCompose(cleaned -> request.sourceCleanup().cleanup())
                .thenCompose(cleaned -> request.source()
                    .discardInitialAfterCommit())
                .thenRun(() -> recordActorHandoffs(
                    (int) stage.participants().stream()
                        .filter(participant -> participant.objectKind() == 1)
                        .count())));
        return operation.exceptionallyCompose(failure -> {
            Throwable original = unwrap(failure);
            if (request.source().relayBoundaryCommitted()) {
                return CompletableFuture.failedFuture(original);
            }
            return request.client().abort(
                    stage.targetNodeRid(),
                    stage.fence(),
                    request.timeout())
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(abortFailure -> abortFailure == null
                    ? request.source().abortPrecommit()
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

    private static void recordActorHandoffs(int count) {
        for (int index = 0; index < count; index++) {
            systems.zlink.framework.runtime.internal.metrics
                .ZLinkRuntimeMetrics.increment(
                    "zlink.drain.actors.handed_off",
                    Map.of());
        }
    }

    record RemoteRequest(
        ZLinkUserSpotRetireSourceBuilder.PreparedSource source,
        ZLinkRelocationTransitionClient client,
        Duration timeout,
        SourceCleanup sourceCleanup) {
        RemoteRequest {
            Objects.requireNonNull(source, "source");
            Objects.requireNonNull(client, "client");
            Objects.requireNonNull(timeout, "timeout");
            Objects.requireNonNull(sourceCleanup, "sourceCleanup");
            if (timeout.isZero() || timeout.isNegative()) {
                throw new IllegalArgumentException(
                    "remote Retire timeout must be positive");
            }
        }

    }

    @FunctionalInterface
    interface SourceCleanup {
        CompletionStage<Void> cleanup();
    }
}
