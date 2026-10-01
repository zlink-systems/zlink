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

final class ZLinkProviderRelocationRepositoryTest {
    @Test
    void uncertainReadbackObservesTheOriginalOperationCancellation() {
        var cancelled = new java.util.concurrent.atomic.AtomicBoolean();
        var provider = new LostResponseProvider(() -> cancelled.set(true));
        var repository = new ZLinkProviderRelocationRepository(provider);
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                repository
                                        .put(
                                                new byte[] {1, 2, 3},
                                                Duration.ofMinutes(5),
                                                cancelled::get)
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

    @Test
    void missingReadbackRestoresTheSameReference() {
        var provider = new RetryProvider(0, false);
        var stored =
                new ZLinkProviderRelocationRepository(provider)
                        .put(new byte[] {1}, Duration.ofMinutes(5), () -> false)
                        .toCompletableFuture()
                        .join();
        assertEquals(provider.firstReference, stored.reference());
        assertEquals(2, provider.puts);
    }

    @Test
    void conflictAndDifferentReadbackAllocateANewReference() {
        for (boolean differentReadback : new boolean[] {false, true}) {
            var provider = new RetryProvider(1, differentReadback);
            var stored =
                    new ZLinkProviderRelocationRepository(provider)
                            .put(new byte[] {1}, Duration.ofMinutes(5), () -> false)
                            .toCompletableFuture()
                            .join();
            assertNotEquals(provider.firstReference, stored.reference());
            assertEquals(2, provider.puts);
        }
    }

    @Test
    void referenceConflictsHaveNoAttemptLimit() {
        var provider = new RetryProvider(256, false);
        var stored =
                new ZLinkProviderRelocationRepository(provider)
                        .put(new byte[] {1}, Duration.ofMinutes(5), () -> false)
                        .toCompletableFuture()
                        .join();
        assertNotEquals(provider.firstReference, stored.reference());
        assertEquals(257, provider.puts);
    }

    @Test
    void readbackRetriesStopWhenTheOriginalOperationIsCancelled() {
        var provider = new RetryProvider(Integer.MAX_VALUE, true);
        var cancelled = new java.util.concurrent.atomic.AtomicBoolean();
        provider.onPut =
                () -> {
                    if (provider.puts == 3) cancelled.set(true);
                };
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                new ZLinkProviderRelocationRepository(provider)
                                        .put(new byte[] {1}, Duration.ofMinutes(5), cancelled::get)
                                        .toCompletableFuture()
                                        .join());
        assertTrue(failure.getCause() instanceof CancellationException);
        assertTrue(provider.puts > 1);
    }

    private static final class RetryProvider implements ZLinkRelocationStore {
        private final int conflicts;
        private final boolean differentReadback;
        private int puts;
        private String firstReference;
        private Runnable onPut = () -> {};

        RetryProvider(int conflicts, boolean differentReadback) {
            this.conflicts = conflicts;
            this.differentReadback = differentReadback;
        }

        public CompletionStage<ZLinkBlobPutResult> put(
                ZLinkBlobReference reference,
                byte[] payload,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            if (cancellation.isCancellationRequested())
                return CompletableFuture.failedFuture(
                        new CancellationException("operation expired"));
            puts++;
            onPut.run();
            if (firstReference == null) firstReference = reference.value();
            Instant now = Instant.now();
            if (puts <= conflicts) {
                return differentReadback
                        ? CompletableFuture.failedFuture(new IllegalStateException("lost response"))
                        : CompletableFuture.completedFuture(new ZLinkBlobConflict(now));
            }
            if (puts == 1)
                return CompletableFuture.failedFuture(new IllegalStateException("lost response"));
            return CompletableFuture.completedFuture(new ZLinkBlobStored(now.plus(retention), now));
        }

        public CompletionStage<ZLinkBlobReadResult> read(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            if (cancellation.isCancellationRequested())
                return CompletableFuture.failedFuture(
                        new CancellationException("operation expired"));
            Instant now = Instant.now();
            return CompletableFuture.completedFuture(
                    differentReadback
                            ? new ZLinkBlobFound(new byte[] {2}, now.plusSeconds(300), now)
                            : new ZLinkBlobMissing(now));
        }

        public CompletionStage<ZLinkBlobRenewResult> renew(
                ZLinkBlobReference reference,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(new ZLinkBlobRenewMissing(Instant.now()));
        }

        public CompletionStage<Void> delete(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class LostResponseProvider implements ZLinkRelocationStore {
        private final Map<String, byte[]> values = new ConcurrentHashMap<>();
        private boolean loseNext = true;
        private boolean readCancellationRequested;
        private final Runnable onLostResponse;

        LostResponseProvider() {
            this(() -> {});
        }

        LostResponseProvider(Runnable onLostResponse) {
            this.onLostResponse = onLostResponse;
        }

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
                onLostResponse.run();
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
