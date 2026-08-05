package systems.zlink.framework.locations.redis;

import java.util.Objects;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;

/**
 * Redis provider for opaque Framework-owned Location Store records.
 */
public final class ZLinkRedisLocationStore
    implements ZLinkLocationStore, AutoCloseable {
    private final ZLinkRedisLocationConnection connection;
    private final ZLinkRedisOpaqueLocationStore store;

    public ZLinkRedisLocationStore(ZLinkRedisLocationOptions options) {
        ZLinkRedisLocationOptions validated =
            Objects.requireNonNull(options, "options");
        validated.validate();
        ZLinkRedisLocationKeys keys =
            new ZLinkRedisLocationKeys(validated.keyPrefix());
        this.connection = new ZLinkRedisLocationConnection(
            validated,
            keys.schemaKey());
        this.store = new ZLinkRedisOpaqueLocationStore(connection, keys);
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation) {
        return store.read(key, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation) {
        return store.write(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation) {
        return store.scan(request, cancellation);
    }

    @Override
    public void close() {
        connection.closeAsync().toCompletableFuture().join();
    }
}
