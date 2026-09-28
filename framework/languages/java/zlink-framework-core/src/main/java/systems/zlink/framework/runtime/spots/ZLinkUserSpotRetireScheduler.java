package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Owns the source-side execution order for a User Spot relocation. */
final class ZLinkUserSpotRetireScheduler {
    CompletionStage<Void> executeRemote(
            RemoteRequest request, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpotRetireControl.StageRequest stage = request.source().stageRequest();
        return ZLinkRelocationHandOff.run(
                        request.client(),
                        stage,
                        () ->
                                request.source()
                                        .relayCapturedIngress(request.client(), request.timeout()),
                        request.timeout(),
                        request.restoreDeadline())
                .handle(
                        (settlement, failure) ->
                                failure == null
                                        ? settle(request, stage, settlement)
                                        : abortBeforeRelayReady(request, stage, unwrap(failure)))
                .thenCompose(outcome -> outcome);
    }

    /** Applies the settled authority of the unit (spec 28 §4.4, spec 30 §13). */
    private static CompletionStage<Void> settle(
            RemoteRequest request,
            ZLinkSpotRetireControl.StageRequest stage,
            ZLinkRelocationTransitionClient.Settlement settlement) {
        var source = request.source();
        return switch (settlement) {
            case TARGET_COMMITTED -> {
                source.completeSourceBarrierCommit();
                yield request.sourceCleanup()
                        .cleanup()
                        .thenCompose(cleaned -> source.discardInitialAfterCommit())
                        .thenRun(() -> recordActorHandoffs(stage));
            }
            case SOURCE_PRESERVED ->
                    source.abortPrecommit()
                            .thenCompose(
                                    ignored ->
                                            CompletableFuture.failedFuture(
                                                    new ZLinkUserSpotRetireRuntime
                                                            .RelocationBlockedException(
                                                            ZLinkFrameworkRelocationReason
                                                                    .RELOCATION_FAILED,
                                                            "Relocation source Preserve fence won: "
                                                                    + stage.spotId())));
            case SOURCE_LEASE_EXPIRED ->
                    source.discardAfterSourceLeaseExpiry(request.sourceCleanup())
                            .thenCompose(
                                    ignored ->
                                            CompletableFuture.failedFuture(
                                                    new ZLinkUserSpotRetireRuntime
                                                            .RelocationAuthorityErrorException(
                                                            "Relocation source owner lease expired"
                                                                    + " before the Preserve fence: "
                                                                    + stage.spotId())));
        };
    }

    /** An explicit failure before relay readiness restores the source (spec 28 §4.2). */
    private static CompletionStage<Void> abortBeforeRelayReady(
            RemoteRequest request, ZLinkSpotRetireControl.StageRequest stage, Throwable original) {
        return request.client()
                .abort(stage.targetNodeRid(), stage.fence(), request.timeout())
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(
                        abortFailure ->
                                abortFailure == null
                                        ? request.source()
                                                .abortPrecommit()
                                                .handle(
                                                        (ignored, sourceFailure) -> {
                                                            if (sourceFailure != null) {
                                                                original.addSuppressed(
                                                                        unwrap(sourceFailure));
                                                            }
                                                            throw new CompletionException(original);
                                                        })
                                        : CompletableFuture.failedFuture(
                                                withSuppressed(original, unwrap(abortFailure))));
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static CompletionException withSuppressed(Throwable original, Throwable suppressed) {
        original.addSuppressed(suppressed);
        return new CompletionException(original);
    }

    private static void recordActorHandoffs(ZLinkSpotRetireControl.StageRequest stage) {
        long count =
                stage.participants().stream()
                        .filter(participant -> participant.objectKind() == 1)
                        .count();
        for (long index = 0; index < count; index++) {
            systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.increment(
                    "zlink.drain.actors.handed_off", Map.of());
        }
    }

    record RemoteRequest(
            ZLinkUserSpotRetireSourceBuilder.PreparedSource source,
            ZLinkRelocationTransitionClient client,
            Duration timeout,
            java.time.Instant restoreDeadline,
            SourceCleanup sourceCleanup) {
        RemoteRequest {
            Objects.requireNonNull(source, "source");
            Objects.requireNonNull(client, "client");
            Objects.requireNonNull(timeout, "timeout");
            Objects.requireNonNull(restoreDeadline, "restoreDeadline");
            Objects.requireNonNull(sourceCleanup, "sourceCleanup");
            if (timeout.isZero() || timeout.isNegative()) {
                throw new IllegalArgumentException("remote Retire timeout must be positive");
            }
        }
    }

    @FunctionalInterface
    interface SourceCleanup {
        CompletionStage<Void> cleanup();
    }
}
