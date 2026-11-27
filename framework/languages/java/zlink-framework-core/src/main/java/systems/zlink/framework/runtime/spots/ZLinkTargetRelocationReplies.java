package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkTargetRelocationReplies {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkLocationRepository locations;
    private final ZLinkLocationOwnerToken writer;
    private final Consumer<Runnable> resubmit;
    private final List<ZLinkRelocationReplyRow> acceptedRows;

    ZLinkTargetRelocationReplies(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkLocationRepository locations,
            Consumer<Runnable> resubmit,
            List<ZLinkRelocationReplyRow> acceptedRows) {
        this.coordinator = coordinator;
        this.locations = locations;
        this.resubmit = resubmit;
        writer =
                new ZLinkLocationOwnerToken(
                        request.targetOwnerId(), request.targetOwnerLeaseGeneration());
        this.acceptedRows = List.copyOf(acceptedRows);
    }

    List<ZLinkRelocationReplyRow> acceptedRows() {
        return acceptedRows;
    }

    CompletionStage<Void> complete(
            ZLinkRelocationReplyRow result,
            Supplier<ZLinkRelocationReplyRow> failure,
            Function<ZLinkRelocationReplyRow, CompletionStage<String>> send) {
        var bounded = result.boundedResult(failure);
        return retry(() -> locations.updateRelocationReply(bounded, OPEN))
                .thenCompose(stored -> retry(() -> deliver(stored, send)));
    }

    private CompletionStage<Void> deliver(
            ZLinkRelocationReplyRow stored,
            Function<ZLinkRelocationReplyRow, CompletionStage<String>> send) {
        if (stored.terminal()) return CompletableFuture.completedFuture(null);
        var source = stored.source();
        return locations
                .readOwnerLease(source.ownerId())
                .thenCompose(
                        lease ->
                                lease instanceof ZLinkOwnerLeaseFound found
                                                && found.isLiveFor(
                                                        source.ownerId(),
                                                        Long.parseUnsignedLong(
                                                                source.leaseGeneration()))
                                        ? send.apply(stored)
                                        : CompletableFuture.completedFuture(
                                                ZLinkRelocationReplyRow.SOURCE_LEASE_EXPIRED))
                .thenCompose(
                        state ->
                                retry(
                                        () ->
                                                locations.updateRelocationReply(
                                                        stored.withState(state), OPEN)))
                .thenAccept(ignored -> {});
    }

    private <T> CompletionStage<T> retry(Supplier<CompletionStage<T>> operation) {
        var result = new CompletableFuture<T>();
        attempt(operation, result);
        return result;
    }

    private <T> void attempt(Supplier<CompletionStage<T>> operation, CompletableFuture<T> result) {
        CompletableFuture.completedFuture(null)
                .thenCompose(ignored -> operation.get())
                .whenComplete(
                        (value, failure) -> {
                            if (failure == null) {
                                result.complete(value);
                                return;
                            }
                            Throwable cause = failure;
                            while (cause instanceof CompletionException
                                    && cause.getCause() != null) {
                                cause = cause.getCause();
                            }
                            if (cause instanceof ZLinkFrameworkException framework
                                    && (framework.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE
                                            || framework.kind()
                                                    == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED)) {
                                resubmit.accept(
                                        () -> {
                                            if (coordinator.isTargetLeaseValid(writer)) {
                                                attempt(operation, result);
                                            } else {
                                                result.completeExceptionally(framework);
                                            }
                                        });
                            } else {
                                result.completeExceptionally(cause);
                            }
                        });
    }
}
