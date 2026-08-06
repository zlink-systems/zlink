package systems.zlink.e2e.kotlin.discoveryregistryha.consumer;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;

public final class DelayableLocationStore implements ZLinkLocationStore {
    private final ZLinkLocationStore inner;
    private final LocationStoreDelayState delayState;

    public DelayableLocationStore(ZLinkLocationStore inner, LocationStoreDelayState delayState) {
        this.inner = inner;
        this.delayState = delayState;
    }

    private <T> CompletionStage<T> delayed(Supplier<CompletionStage<T>> action) {
        int delay = delayState.delayMilliseconds();
        if (delay <= 0) return action.get();
        return CompletableFuture.runAsync(
                () -> { },
                CompletableFuture.delayedExecutor(delay, TimeUnit.MILLISECONDS))
            .thenCompose(ignored -> action.get());
    }

    @Override public CompletionStage<ZLinkStoreReadResult> read(ZLinkStoreKey key, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.read(key, cancellation)); }
    @Override public CompletionStage<ZLinkStoreWriteResult> write(ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.write(request, cancellation)); }
    @Override public CompletionStage<ZLinkStoreScanResult> scan(ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) { return delayed(() -> inner.scan(request, cancellation)); }
}
