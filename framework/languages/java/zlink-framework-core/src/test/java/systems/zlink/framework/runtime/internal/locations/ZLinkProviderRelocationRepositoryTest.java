package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkBlobAlreadyStored;
import systems.zlink.framework.locationprovider.ZLinkBlobConflict;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobMissing;
import systems.zlink.framework.locationprovider.ZLinkBlobPutResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReadResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewMissing;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewResult;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;
import systems.zlink.framework.locationprovider.ZLinkRelocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;

import java.time.Duration;
import java.time.Instant;
import java.util.Arrays;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkProviderRelocationRepositoryTest {
    @Test
    void uncertainReadbackObservesTheOriginalOperationCancellation() {
        var provider = new LostResponseProvider();
        var repository = new ZLinkProviderRelocationRepository(provider);
        var cancellationChecks = new AtomicInteger();
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                repository
                                        .put(
                                                new byte[] {1, 2, 3},
                                                Duration.ofMinutes(5),
                                                () -> cancellationChecks.getAndIncrement() > 0)
                                        .toCompletableFuture()
                                        .join());
        assertTrue(provider.readCancellationRequested);
        assertEquals("operation expired", failure.getCause().getMessage());
    }

    @Test
    void lostPutResponseIsReconciledByExactReferenceAndBytes() throws Exception {
        var provider = new LostResponseProvider();
        var repository = new ZLinkProviderRelocationRepository(provider);
        var first =
                repository
                        .put(new byte[] {1, 2, 3}, Duration.ofMinutes(5), () -> false)
                        .toCompletableFuture()
                        .get();
        var second =
                repository
                        .put(new byte[] {1, 2, 3}, Duration.ofMinutes(5), () -> false)
                        .toCompletableFuture()
                        .get();

        assertNotEquals(first.reference(), second.reference());
        var found =
                (ZLinkRelocationFound)
                        repository.get(first.reference(), () -> false).toCompletableFuture().get();
        assertArrayEquals(new byte[] {1, 2, 3}, found.payload());
    }

    private static final class LostResponseProvider implements ZLinkRelocationStore {
        private final Map<String, byte[]> values = new ConcurrentHashMap<>();
        private boolean loseNext = true;
        private boolean readCancellationRequested;

        @Override
        public CompletionStage<ZLinkBlobPutResult> put(
                ZLinkBlobReference reference,
                byte[] payload,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            assertFalse(cancellation.isCancellationRequested());
            Instant now = Instant.now();
            byte[] previous = values.putIfAbsent(reference.value(), payload.clone());
            if (previous != null) {
                return CompletableFuture.completedFuture(
                        Arrays.equals(previous, payload)
                                ? new ZLinkBlobAlreadyStored(now.plus(retention), now)
                                : new ZLinkBlobConflict(now));
            }
            if (loseNext) {
                loseNext = false;
                return CompletableFuture.failedFuture(new IllegalStateException("lost response"));
            }
            return CompletableFuture.completedFuture(new ZLinkBlobStored(now.plus(retention), now));
        }

        @Override
        public CompletionStage<ZLinkBlobReadResult> read(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            readCancellationRequested = cancellation.isCancellationRequested();
            if (readCancellationRequested) {
                return CompletableFuture.failedFuture(
                        new CancellationException("operation expired"));
            }
            Instant now = Instant.now();
            byte[] payload = values.get(reference.value());
            return CompletableFuture.completedFuture(
                    payload == null
                            ? new ZLinkBlobMissing(now)
                            : new ZLinkBlobFound(payload.clone(), now.plusSeconds(300), now));
        }

        @Override
        public CompletionStage<ZLinkBlobRenewResult> renew(
                ZLinkBlobReference reference,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(new ZLinkBlobRenewMissing(Instant.now()));
        }

        @Override
        public CompletionStage<Void> delete(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            values.remove(reference.value());
            return CompletableFuture.completedFuture(null);
        }
    }
}
