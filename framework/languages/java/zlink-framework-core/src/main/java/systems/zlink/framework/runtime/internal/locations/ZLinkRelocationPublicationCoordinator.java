package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.Arrays;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.zip.CRC32C;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/**
 * Publishes an immutable relocation root through one Location authority CAS.
 */
public final class ZLinkRelocationPublicationCoordinator {
    private static final Duration RETENTION = Duration.ofHours(24);

    private final ZLinkLocationRepository authority;
    private final ZLinkRelocationStore relocation;

    public ZLinkRelocationPublicationCoordinator(
        ZLinkLocationRepository authority,
        ZLinkRelocationStore relocation) {
        this.authority = Objects.requireNonNull(authority, "authority");
        this.relocation = Objects.requireNonNull(relocation, "relocation");
    }

    public CompletionStage<Publication> publish(
        String authorityKey,
        ZLinkAuthorityExpectation expectation,
        byte[] root,
        Function<ZLinkRelocationStored, ZLinkAuthorityPut> mutation,
        ZLinkStoreCancellation cancellation) {
        byte[] rootSnapshot = Objects.requireNonNull(root, "root").clone();
        Objects.requireNonNull(mutation, "mutation");
        return relocation.put(rootSnapshot, RETENTION, cancellation)
            .thenCompose(stored -> verifyStored(stored, rootSnapshot, cancellation)
                .thenCompose(ignored -> publishStored(
                    authorityKey,
                    expectation,
                    stored,
                    mutation.apply(stored),
                    cancellation)));
    }

    public CompletionStage<byte[]> restore(
        String reference,
        long checksumCrc32c,
        ZLinkStoreCancellation cancellation) {
        return relocation.get(reference, cancellation)
            .thenCompose(result -> {
                if (!(result instanceof ZLinkRelocationFound found)) {
                    return CompletableFuture.failedFuture(
                        new RelocationDataLostException(
                            "published relocation root is missing: " + reference));
                }
                byte[] payload = found.payload();
                if (crc32c(payload) != checksumCrc32c) {
                    return CompletableFuture.failedFuture(
                        new RelocationDataLostException(
                            "published relocation root checksum mismatch: " + reference));
                }
                return CompletableFuture.completedFuture(payload);
            });
    }

    private CompletionStage<Void> verifyStored(
        ZLinkRelocationStored stored,
        byte[] expected,
        ZLinkStoreCancellation cancellation) {
        if (stored.checksumCrc32c() != crc32c(expected)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "Relocation Store returned a different checksum"));
        }
        return relocation.get(stored.reference(), cancellation)
            .thenCompose(result -> result instanceof ZLinkRelocationFound found
                    && Arrays.equals(found.payload(), expected)
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Relocation Store did not return the immutable root")));
    }

    private CompletionStage<Publication> publishStored(
        String authorityKey,
        ZLinkAuthorityExpectation expectation,
        ZLinkRelocationStored stored,
        ZLinkAuthorityPut mutation,
        ZLinkStoreCancellation cancellation) {
        byte[] expectedAuthorityPayload = mutation.payload();
        CompletionStage<ZLinkAuthorityWriteResult> write;
        try {
            write = authority.compareExchange(
                authorityKey,
                expectation,
                mutation,
                cancellation);
        } catch (RuntimeException failure) {
            write = CompletableFuture.failedFuture(failure);
        }
        return write.handle((result, failure) -> new WriteAttempt(result, failure))
            .thenCompose(attempt -> {
                if (attempt.failure == null) {
                    if (attempt.result instanceof ZLinkAuthorityStored storedAuthority) {
                        return CompletableFuture.completedFuture(
                            new Publication(
                                stored.reference(),
                                stored.checksumCrc32c(),
                                storedAuthority.storeVersion(),
                                storedAuthority.objectGeneration(),
                                storedAuthority.authorityOwnerGeneration(),
                                false));
                    }
                    return deleteOrphan(stored.reference(), cancellation)
                        .thenCompose(ignored -> CompletableFuture.failedFuture(
                            new AuthorityConflictException(attempt.result)));
                }
                return reconcile(
                    authorityKey,
                    stored,
                    expectedAuthorityPayload,
                    cancellation,
                    unwrap(attempt.failure));
            });
    }

    private CompletionStage<Publication> reconcile(
        String authorityKey,
        ZLinkRelocationStored stored,
        byte[] expectedAuthorityPayload,
        ZLinkStoreCancellation cancellation,
        Throwable originalFailure) {
        return authority.read(authorityKey, cancellation)
            .handle((result, readFailure) -> new ReadAttempt(result, readFailure))
            .thenCompose(attempt -> {
                if (attempt.failure != null) {
                    return CompletableFuture.failedFuture(originalFailure);
                }
                if (attempt.result instanceof ZLinkAuthoritySnapshot snapshot
                    && Arrays.equals(snapshot.payload(), expectedAuthorityPayload)) {
                    return CompletableFuture.completedFuture(new Publication(
                        stored.reference(),
                        stored.checksumCrc32c(),
                        snapshot.storeVersion(),
                        snapshot.objectGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        true));
                }
                return deleteOrphan(stored.reference(), cancellation)
                    .thenCompose(ignored ->
                        CompletableFuture.failedFuture(originalFailure));
            });
    }

    private CompletionStage<Void> deleteOrphan(
        String reference,
        ZLinkStoreCancellation cancellation) {
        return relocation.delete(reference, cancellation).thenApply(ignored -> null);
    }

    private static long crc32c(byte[] payload) {
        CRC32C checksum = new CRC32C();
        checksum.update(payload, 0, payload.length);
        return checksum.getValue();
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    public record Publication(
        String reference,
        long checksumCrc32c,
        String authorityStoreVersion,
        long objectGeneration,
        long authorityOwnerGeneration,
        boolean reconciled) {
    }

    public static final class AuthorityConflictException extends RuntimeException {
        private final ZLinkAuthorityWriteResult result;

        AuthorityConflictException(ZLinkAuthorityWriteResult result) {
            super("relocation authority publication was rejected");
            this.result = result;
        }

        public ZLinkAuthorityWriteResult result() {
            return result;
        }
    }

    public static final class RelocationDataLostException extends RuntimeException {
        RelocationDataLostException(String message) {
            super(message);
        }
    }

    private record WriteAttempt(
        ZLinkAuthorityWriteResult result,
        Throwable failure) {
    }

    private record ReadAttempt(
        ZLinkAuthorityReadResult result,
        Throwable failure) {
    }
}
