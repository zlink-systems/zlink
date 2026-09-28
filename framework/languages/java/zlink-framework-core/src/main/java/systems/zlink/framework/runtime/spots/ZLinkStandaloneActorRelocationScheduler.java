package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/**
 * Executes one Entry Spot Actor relocation through the same fenced control channel used by User
 * Spot aggregates.
 */
final class ZLinkStandaloneActorRelocationScheduler {
    CompletionStage<Void> executeRemote(
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource source,
            ZLinkRelocationTransitionClient client,
            Duration timeout,
            Instant restoreDeadline,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(client, "client");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(restoreDeadline, "restoreDeadline");
        Objects.requireNonNull(cancellation, "cancellation");
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "remote Actor relocation timeout must be positive"));
        }

        var request = source.stageRequest();
        return ZLinkRelocationHandOff.run(
                        client,
                        request,
                        () -> source.relayCapturedIngress(client, timeout),
                        timeout,
                        restoreDeadline)
                .handle(
                        (settlement, failure) ->
                                failure == null
                                        ? settle(source, request, settlement)
                                        : abortBeforeRelayReady(
                                                source, client, request, timeout, unwrap(failure)))
                .thenCompose(outcome -> outcome);
    }

    /** Applies the settled authority of the unit (spec 28 §4.4, spec 30 §13). */
    private static CompletionStage<Void> settle(
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource source,
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkRelocationTransitionClient.Settlement settlement) {
        return switch (settlement) {
            case TARGET_COMMITTED -> {
                source.completeSourceQueueCommit();
                yield source.cleanupLocal()
                        .thenCompose(cleaned -> source.discardInitialAfterCommit())
                        .thenRun(
                                () ->
                                        systems.zlink.framework.runtime.internal.metrics
                                                .ZLinkRuntimeMetrics.increment(
                                                "zlink.drain.actors.handed_off", Map.of()));
            }
            case SOURCE_PRESERVED ->
                    source.abort()
                            .thenCompose(
                                    ignored ->
                                            CompletableFuture.failedFuture(
                                                    new ZLinkUserSpotRetireRuntime
                                                            .RelocationBlockedException(
                                                            ZLinkFrameworkRelocationReason
                                                                    .RELOCATION_FAILED,
                                                            "Relocation source Preserve fence won: "
                                                                    + request.spotId())));
            case SOURCE_LEASE_EXPIRED ->
                    source.discardAfterSourceLeaseExpiry()
                            .thenCompose(
                                    ignored ->
                                            CompletableFuture.failedFuture(
                                                    new ZLinkUserSpotRetireRuntime
                                                            .RelocationAuthorityErrorException(
                                                            "Relocation source owner lease expired"
                                                                    + " before the Preserve fence: "
                                                                    + request.spotId())));
        };
    }

    /** An explicit failure before relay readiness restores the source (spec 28 §4.2). */
    private static CompletionStage<Void> abortBeforeRelayReady(
            ZLinkStandaloneActorRelocationSourceBuilder.PreparedSource source,
            ZLinkRelocationTransitionClient client,
            ZLinkSpotRetireControl.StageRequest request,
            Duration timeout,
            Throwable original) {
        return client.abort(request.targetNodeRid(), request.fence(), timeout)
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(
                        abortFailure ->
                                abortFailure == null
                                        ? source.abort()
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
}
