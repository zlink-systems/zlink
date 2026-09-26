package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.time.Instant;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import java.util.logging.Logger;

/**
 * The one source-side hand-off order every relocation unit uses (spec 28 §4.4): Restore until relay
 * readiness, ordered relay, one cutover submit, then authority settlement. A failure before relay
 * readiness completes the returned stage exceptionally so the caller restores the source. After
 * relay readiness the outcome is only the settlement — a relay or cutover submit terminal is
 * neither success nor failure and never reopens the source.
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
                        ready ->
                                relayCapturedIngress
                                        .get()
                                        .thenCompose(
                                                ignored ->
                                                        client.publish(
                                                                targetRid,
                                                                request.fence(),
                                                                timeout))
                                        .handle(
                                                (ignored, failure) -> {
                                                    if (failure != null) {
                                                        LOGGER.warning(
                                                                "Relocation relay or CUTOVER"
                                                                        + " submit failed;"
                                                                        + " authority settlement"
                                                                        + " decides the unit: "
                                                                        + unwrap(failure));
                                                    }
                                                    return null;
                                                })
                                        .thenCompose(
                                                ignored ->
                                                        client.settle(
                                                                targetRid,
                                                                request.fence(),
                                                                restoreDeadline)));
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
