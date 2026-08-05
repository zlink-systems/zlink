package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.time.Duration;
import java.time.Instant;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

final class ZLinkProviderOwnerLeaseRepositoryTest {
    @Test
    void generationsRemainMonotonicAcrossReleaseAndReclaim()
        throws Exception {
        var repository = new ZLinkProviderOwnerLeaseRepository(
            new AtomicProvider());

        var first = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            repository.claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get());
        assertEquals(
            ZLinkOwnerLeaseReleaseResult.RELEASED,
            repository.release(first.token())
                .toCompletableFuture().get());
        var second = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            repository.claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get());

        assertEquals(
            first.token().leaseGeneration() + 1,
            second.token().leaseGeneration());
    }

    @Test
    void exactLeaseGenerationFencesRenewAndRelease() throws Exception {
        var repository = new ZLinkProviderOwnerLeaseRepository(
            new AtomicProvider());
        var claimed = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            repository.claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get());
        var stale = new ZLinkLocationOwnerToken(
            claimed.token().ownerId(),
            claimed.token().leaseGeneration() + 1);

        assertInstanceOf(
            ZLinkOwnerLeaseRenewStale.class,
            repository.renew(stale, Duration.ofMinutes(1))
                .toCompletableFuture().get());
        assertEquals(
            ZLinkOwnerLeaseReleaseResult.STALE,
            repository.release(stale).toCompletableFuture().get());
        assertInstanceOf(
            ZLinkOwnerLeaseFound.class,
            repository.read("owner-a").toCompletableFuture().get());
    }

    private static final class AtomicProvider
        implements systems.zlink.framework.locationprovider
            .ZLinkLocationStore {
        private final Map<ZLinkStoreKey, Entry> rows = new HashMap<>();
        private long version;

        @Override
        public synchronized CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
            Instant now = Instant.now();
            Entry entry = rows.get(key);
            if (entry != null
                && entry.expiresAt() != null
                && !entry.expiresAt().isAfter(now)) {
                rows.remove(key);
                entry = null;
            }
            return completed(entry == null
                ? new ZLinkStoreReadMissing(now)
                : new ZLinkStoreReadFound(new ZLinkStoreValue(
                    entry.bytes(),
                    entry.version(),
                    entry.expiresAt(),
                    now)));
        }

        @Override
        public synchronized CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
            Instant now = Instant.now();
            for (ZLinkStoreCondition condition : request.conditions()) {
                ZLinkStoreKey key =
                    condition instanceof ZLinkStoreMissingCondition missing
                        ? missing.key()
                        : ((ZLinkStoreVersionCondition) condition).key();
                Entry current = rows.get(key);
                boolean matches =
                    condition instanceof ZLinkStoreMissingCondition
                        ? current == null
                        : current != null
                            && current.version().equals(
                                ((ZLinkStoreVersionCondition) condition)
                                    .expected());
                if (!matches) {
                    return completed(new ZLinkStoreWriteConflict(now));
                }
            }
            Map<ZLinkStoreKey, ZLinkStoreVersion> versions =
                new LinkedHashMap<>();
            for (ZLinkStoreMutation mutation : request.mutations()) {
                if (mutation instanceof ZLinkStoreDelete delete) {
                    rows.remove(delete.key());
                    continue;
                }
                ZLinkStorePut put = (ZLinkStorePut) mutation;
                var next = new ZLinkStoreVersion(
                    Long.toString(++version));
                rows.put(
                    put.key(),
                    new Entry(
                        put.bytes(),
                        next,
                        put.retention() == null
                            ? null
                            : now.plus(put.retention())));
                versions.put(put.key(), next);
            }
            return completed(new ZLinkStoreWriteApplied(
                Map.copyOf(versions), now));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
            throw new UnsupportedOperationException();
        }

        private static <T> CompletionStage<T> completed(T value) {
            return CompletableFuture.completedFuture(value);
        }

        private record Entry(
            byte[] bytes,
            ZLinkStoreVersion version,
            Instant expiresAt) {
            private Entry {
                bytes = bytes.clone();
            }

            @Override
            public byte[] bytes() {
                return bytes.clone();
            }
        }
    }
}
