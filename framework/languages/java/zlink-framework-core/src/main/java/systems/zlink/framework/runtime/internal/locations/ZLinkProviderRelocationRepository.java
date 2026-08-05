package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;
import systems.zlink.framework.locationprovider.ZLinkBlobAlreadyStored;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewed;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

/**
 * Keeps relocation identity and retry policy inside the Framework.
 */
public final class ZLinkProviderRelocationRepository
    implements ZLinkRelocationStore {
    private final systems.zlink.framework.locationprovider.ZLinkRelocationStore
        provider;

    public ZLinkProviderRelocationRepository(
        systems.zlink.framework.locationprovider.ZLinkRelocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    @Override
    public CompletionStage<ZLinkRelocationStored> put(
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        byte[] bytes = Objects.requireNonNull(payload, "payload").clone();
        var reference = new ZLinkBlobReference(UUID.randomUUID().toString());
        CRC32C crc = new CRC32C();
        crc.update(bytes, 0, bytes.length);
        long checksum = crc.getValue();
        return provider.put(
                reference,
                bytes,
                retention,
                cancellation::isCancellationRequested)
            .handle((result, failure) -> failure == null
                ? completed(reference, checksum, result)
                : reconcile(reference, bytes, checksum, unwrap(failure)))
            .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkRelocationStored> reconcile(
        ZLinkBlobReference reference,
        byte[] expected,
        long checksum,
        Throwable originalFailure) {
        // A committed write can lose its response. Reconciliation therefore
        // ignores the caller cancellation that ended the original operation.
        return provider.read(reference, () -> false)
            .toCompletableFuture()
            .orTimeout(5, java.util.concurrent.TimeUnit.SECONDS)
            .handle((result, failure) -> {
                if (failure == null
                    && result instanceof ZLinkBlobFound found
                    && Arrays.equals(expected, found.bytes())) {
                    return stored(
                        reference,
                        checksum,
                        found.expiresAt(),
                        found.storeNow());
                }
                throw new CompletionException(originalFailure);
            });
    }

    private static CompletionStage<ZLinkRelocationStored> completed(
        ZLinkBlobReference reference,
        long checksum,
        systems.zlink.framework.locationprovider.ZLinkBlobPutResult result) {
        if (result instanceof ZLinkBlobStored stored) {
            return CompletableFuture.completedFuture(stored(
                reference, checksum, stored.expiresAt(), stored.storeNow()));
        }
        if (result instanceof ZLinkBlobAlreadyStored stored) {
            return CompletableFuture.completedFuture(stored(
                reference, checksum, stored.expiresAt(), stored.storeNow()));
        }
        return CompletableFuture.failedFuture(new IllegalStateException(
            "provider rejected a newly issued relocation reference"));
    }

    @Override
    public CompletionStage<ZLinkRelocationReadResult> get(
        String reference,
        ZLinkStoreCancellation cancellation) {
        return provider.read(
                new ZLinkBlobReference(reference),
                cancellation::isCancellationRequested)
            .thenApply(result -> result instanceof ZLinkBlobFound found
                ? new ZLinkRelocationFound(found.bytes())
                : new ZLinkRelocationMissing());
    }

    @Override
    public CompletionStage<ZLinkRelocationRenewResult> renew(
        String reference,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        return provider.renew(
                new ZLinkBlobReference(reference),
                retention,
                cancellation::isCancellationRequested)
            .thenApply(result -> result instanceof ZLinkBlobRenewed renewed
                ? new ZLinkRelocationRenewed(
                    renewed.expiresAt(),
                    renewed.storeNow())
                : new ZLinkRelocationRenewMissing());
    }

    @Override
    public CompletionStage<ZLinkRelocationDeleteResult> delete(
        String reference,
        ZLinkStoreCancellation cancellation) {
        return provider.delete(
                new ZLinkBlobReference(reference),
                cancellation::isCancellationRequested)
            .thenApply(ignored -> ZLinkRelocationDeleteResult.DELETED);
    }

    private static ZLinkRelocationStored stored(
        ZLinkBlobReference reference,
        long checksum,
        java.time.Instant expiresAt,
        java.time.Instant storeNow) {
        return new ZLinkRelocationStored(
            reference.value(), checksum, expiresAt, storeNow);
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion
            && completion.getCause() != null
            ? completion.getCause()
            : failure;
    }
}
