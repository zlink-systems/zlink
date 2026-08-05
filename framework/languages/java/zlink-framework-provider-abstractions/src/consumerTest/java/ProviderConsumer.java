import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locationprovider.*;

final class ProviderConsumer
    implements ZLinkLocationStore, ZLinkRelocationStore {
    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }

    @Override
    public CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation) {
        throw new UnsupportedOperationException();
    }
}
