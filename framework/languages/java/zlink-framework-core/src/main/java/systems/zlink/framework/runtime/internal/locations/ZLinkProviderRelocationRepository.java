package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.ZLinkBlobAlreadyStored;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewed;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.LocationOperation;

import java.time.Duration;
import java.time.Instant;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;

/** Keeps relocation identity and retry policy inside the Framework. */
public final class ZLinkProviderRelocationRepository implements ZLinkRelocationStore {
    private final systems.zlink.framework.locationprovider.ZLinkRelocationStore provider;

    public ZLinkProviderRelocationRepository(
            systems.zlink.framework.locationprovider.ZLinkRelocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    @Override
    public CompletionStage<ZLinkRelocationStored> put(
            byte[] payload, Duration retention, ZLinkStoreCancellation cancellation) {
        byte[] bytes = Objects.requireNonNull(payload, "payload").clone();

        CRC32C crc = new CRC32C();
        crc.update(bytes, 0, bytes.length);
        long checksum = crc.getValue();
        return put(null, bytes, checksum, retention, cancellation);
    }

    private CompletionStage<ZLinkRelocationStored> put(
            ZLinkBlobReference requestedReference,
            byte[] bytes,
            long checksum,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
        ZLinkBlobReference reference =
                requestedReference == null
                        ? new ZLinkBlobReference(UUID.randomUUID().toString())
                        : requestedReference;
        if (cancellation.isCancellationRequested()) {
            return CompletableFuture.failedFuture(
                    new java.util.concurrent.CancellationException("operation expired"));
        }
        return observe(
                        LocationOperation.RELOCATION_PUT,
                        () ->
                                provider.put(
                                        reference,
                                        bytes,
                                        retention,
                                        cancellation::isCancellationRequested))
                .<CompletionStage<ZLinkRelocationStored>>handle(
                        (result, failure) -> {
                            if (failure != null) {
                                return reconcile(
                                        reference, bytes, checksum, retention, cancellation);
                            }
                            if (result instanceof ZLinkBlobStored stored) {
                                return CompletableFuture.completedFuture(
                                        stored(
                                                reference,
                                                checksum,
                                                stored.expiresAt(),
                                                stored.storeNow()));
                            }
                            if (result instanceof ZLinkBlobAlreadyStored stored) {
                                return CompletableFuture.completedFuture(
                                        stored(
                                                reference,
                                                checksum,
                                                stored.expiresAt(),
                                                stored.storeNow()));
                            }
                            return CompletableFuture.completedFuture((Void) null)
                                    .thenComposeAsync(
                                            ignored ->
                                                    put(
                                                            null,
                                                            bytes,
                                                            checksum,
                                                            retention,
                                                            cancellation));
                        })
                .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkRelocationStored> reconcile(
            ZLinkBlobReference reference,
            byte[] expected,
            long checksum,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
        return observe(
                        LocationOperation.RELOCATION_GET,
                        () -> provider.read(reference, cancellation::isCancellationRequested))
                .thenComposeAsync(
                        result -> {
                            if (result instanceof ZLinkBlobFound found) {
                                if (Arrays.equals(expected, found.bytes())) {
                                    return CompletableFuture.completedFuture(
                                            stored(
                                                    reference,
                                                    checksum,
                                                    found.expiresAt(),
                                                    found.storeNow()));
                                }
                                return put(null, expected, checksum, retention, cancellation);
                            }
                            return put(reference, expected, checksum, retention, cancellation);
                        });
    }

    @Override
    public CompletionStage<ZLinkRelocationReadResult> get(
            String reference, ZLinkStoreCancellation cancellation) {
        return observe(
                        LocationOperation.RELOCATION_GET,
                        () ->
                                provider.read(
                                        new ZLinkBlobReference(reference),
                                        cancellation::isCancellationRequested))
                .thenApply(
                        result ->
                                result instanceof ZLinkBlobFound found
                                        ? new ZLinkRelocationFound(found.bytes())
                                        : new ZLinkRelocationMissing());
    }

    @Override
    public CompletionStage<ZLinkRelocationRenewResult> renew(
            String reference, Duration retention, ZLinkStoreCancellation cancellation) {
        return observe(
                        LocationOperation.RELOCATION_PUT,
                        () ->
                                provider.renew(
                                        new ZLinkBlobReference(reference),
                                        retention,
                                        cancellation::isCancellationRequested))
                .thenApply(
                        result ->
                                result instanceof ZLinkBlobRenewed renewed
                                        ? new ZLinkRelocationRenewed(
                                                renewed.expiresAt(), renewed.storeNow())
                                        : new ZLinkRelocationRenewMissing());
    }

    @Override
    public CompletionStage<ZLinkRelocationDeleteResult> delete(
            String reference, ZLinkStoreCancellation cancellation) {
        return observe(
                        LocationOperation.RELOCATION_DELETE,
                        () ->
                                provider.delete(
                                        new ZLinkBlobReference(reference),
                                        cancellation::isCancellationRequested))
                .thenApply(ignored -> ZLinkRelocationDeleteResult.DELETED);
    }

    private static <T> CompletionStage<T> observe(
            LocationOperation operation, java.util.function.Supplier<CompletionStage<T>> call) {
        CompletionStage<T> result;
        try {
            result = call.get();
        } catch (RuntimeException failure) {
            recordFailure(operation, failure);
            throw failure;
        }
        return result.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) recordFailure(operation, failure);
                });
    }

    private static void recordFailure(LocationOperation operation, Throwable failure) {
        while (failure instanceof java.util.concurrent.CompletionException
                && failure.getCause() != null) failure = failure.getCause();
        if (!(failure instanceof java.util.concurrent.CancellationException)) operation.failure();
    }

    private static ZLinkRelocationStored stored(
            ZLinkBlobReference reference, long checksum, Instant expiresAt, Instant storeNow) {
        return new ZLinkRelocationStored(reference.value(), checksum, expiresAt, storeNow);
    }
}
