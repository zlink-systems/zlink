package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.spots.ZLinkUserSpotAggregateStagingOwner.PreparedSpot;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

final class TestPreparedSpot implements PreparedSpot {
    private final Object surface;

    TestPreparedSpot(Object surface) {
        this.surface = surface;
    }

    @Override
    public Object surface() {
        return surface;
    }

    @Override
    public CompletionStage<Void> completeRelocationReady() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Supplier<CompletionStage<Void>>> beginIngressHold() {
        return CompletableFuture.completedFuture(() -> CompletableFuture.completedFuture(null));
    }

    @Override
    public CompletionStage<Void> enqueue(
            byte[] record,
            Supplier<CompletionStage<Void>> turn,
            CompletableFuture<Void> admission) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<Void> publish() {
        throw new UnsupportedOperationException();
    }

    @Override
    public void publishTimers() {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<List<byte[]>> replay(ZLinkSpotAcceptedJournal.Record record) {
        return CompletableFuture.failedFuture(
                new IllegalStateException("staged Spot replay is unavailable"));
    }

    @Override
    public CompletionStage<Void> discard() {
        throw new UnsupportedOperationException();
    }
}
