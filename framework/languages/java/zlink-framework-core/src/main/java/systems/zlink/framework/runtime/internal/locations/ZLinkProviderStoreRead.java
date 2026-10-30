package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Classifies SPI read failures before record decoding. */
final class ZLinkProviderStoreRead {
    private ZLinkProviderStoreRead() {}

    static CompletionStage<ZLinkStoreReadResult> read(
            ZLinkLocationStore provider,
            ZLinkStoreKey key,
            ZLinkStoreCancellation cancellation,
            String recordName) {
        return CompletableFuture.completedFuture(provider)
                .thenCompose(store -> store.read(key, cancellation))
                .handle(
                        (result, failure) -> {
                            if (failure != null) {
                                Throwable cause =
                                        failure instanceof CompletionException completion
                                                        && completion.getCause() != null
                                                ? completion.getCause()
                                                : failure;
                                throw new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        "Location Store failed to read " + recordName,
                                        cause);
                            }
                            return result;
                        });
    }
}
