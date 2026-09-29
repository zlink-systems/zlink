package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.time.Instant;
import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import java.util.logging.Logger;

/**
 * The one source-side hand-off order every relocation unit uses (spec 28 §4.4): Restore until relay
 * readiness, ordered relay, one cutover submit, and authority settlement. A failure before relay
 * readiness completes the returned stage exceptionally so the caller restores the source. After
 * relay readiness the outcome is only the settlement. Relay admission cannot delay the Store's
 * Restore deadline decision; a relay or cutover submit terminal never reopens the source.
 */
final class ZLinkRelocationHandOff {
    private static final Logger LOGGER = Logger.getLogger(ZLinkRelocationHandOff.class.getName());

    private ZLinkRelocationHandOff() {}

    /**
     * Runs one unit. {@code timeout} bounds each control request; {@code restoreDeadline} is the
     * unit's Restore absolute deadline, from which the source settles with its {@code Preserve}
     * fence (spec 01 §10).
     */
    static CompletionStage<ZLinkRelocationTransitionClient.Settlement> run(
            ZLinkRelocationTransitionClient client,
            ZLinkSpotRetireControl.StageRequest request,
            Supplier<CompletionStage<Void>> relayCapturedIngress,
            Duration timeout,
            Instant restoreDeadline) {
        Objects.requireNonNull(client, "client");
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(relayCapturedIngress, "relayCapturedIngress");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(restoreDeadline, "restoreDeadline");
        RoutingId targetRid = request.targetNodeRid();
        return client.stage(targetRid, request, timeout)
                .thenCompose(
                        ready -> {
                            CompletionStage<Void> relay;
                            try {
                                relay = relayCapturedIngress.get();
                            } catch (RuntimeException failure) {
                                relay = CompletableFuture.failedFuture(failure);
                            }
                            CompletionStage<Void> capturedRelay = relay;
                            AtomicReference<CompletionStage<Void>> cutover =
                                    new AtomicReference<>();
                            CompletableFuture<Void> cancelledCutover = new CompletableFuture<>();
                            CompletionStage<Void> boundary =
                                    capturedRelay.thenCompose(
                                            ignored -> {
                                                CompletionStage<Void> submitted =
                                                        client.publish(
                                                                targetRid,
                                                                request.fence(),
                                                                timeout);
                                                if (!cutover.compareAndSet(null, submitted)) {
                                                    submitted.toCompletableFuture().cancel(false);
                                                }
                                                return submitted;
                                            });
                            boundary.whenComplete(
                                    (ignored, failure) -> {
                                        if (failure != null
                                                && !(unwrap(failure)
                                                        instanceof CancellationException)) {
                                            LOGGER.warning(
                                                    "Relocation relay or CUTOVER submit"
                                                            + " failed; authority settlement"
                                                            + " decides the unit: "
                                                            + unwrap(failure));
                                        }
                                    });
                            return client.settle(targetRid, request.fence(), restoreDeadline)
                                    .handle(
                                            (settlement, failure) -> {
                                                if (failure != null
                                                        || settlement
                                                                != ZLinkRelocationTransitionClient
                                                                        .Settlement
                                                                        .TARGET_COMMITTED) {
                                                    capturedRelay
                                                            .toCompletableFuture()
                                                            .cancel(false);
                                                    cancelledCutover.cancel(false);
                                                    CompletionStage<Void> submitted =
                                                            cutover.getAndSet(cancelledCutover);
                                                    if (submitted != null) {
                                                        submitted
                                                                .toCompletableFuture()
                                                                .cancel(false);
                                                    }
                                                    boundary.toCompletableFuture().cancel(false);
                                                    return failure == null
                                                            ? CompletableFuture.completedFuture(
                                                                    settlement)
                                                            : CompletableFuture
                                                                    .<ZLinkRelocationTransitionClient
                                                                                    .Settlement>
                                                                            failedFuture(failure);
                                                }
                                                return boundary.handle(
                                                        (ignored, boundaryFailure) -> settlement);
                                            })
                                    .thenCompose(result -> result);
                        });
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
