package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongSupplier;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;

final class ZLinkOwnerLeaseTracker {
    private final ZLinkLocationRepository store;
    private final Duration pollingInterval;
    private final LongSupplier nanoTime;
    private final Map<String, ObservedLease> observed =
        new ConcurrentHashMap<>();

    ZLinkOwnerLeaseTracker(
        ZLinkLocationRepository store,
        Duration pollingInterval) {
        this(store, pollingInterval, System::nanoTime);
    }

    ZLinkOwnerLeaseTracker(
        ZLinkLocationRepository store,
        Duration pollingInterval,
        LongSupplier nanoTime) {
        this.store = Objects.requireNonNull(store, "store");
        this.pollingInterval = requirePositive(
            pollingInterval,
            "pollingInterval");
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
    }

    CompletionStage<Boolean> isOwnerLive(String ownerId) {
        return remainingAdmissionLifetime(ownerId, 0L)
            .thenApply(remaining -> remaining != null);
    }

    CompletionStage<Duration> remainingAdmissionLifetime(
        String ownerId,
        long expectedGeneration) {
        ObservedLease current = observed.get(ownerId);
        long now = nanoTime.getAsLong();
        if (current != null
            && Duration.ofNanos(now - current.fetchedAtNanos)
                .compareTo(pollingInterval) < 0) {
            Duration remaining = current.remaining(now);
            return CompletableFuture.completedFuture(
                current.matches(expectedGeneration)
                    && remaining.compareTo(Duration.ZERO) > 0
                        ? remaining
                        : null);
        }
        return store.readOwnerLease(ownerId).thenApply(result -> {
            long fetchedAt = nanoTime.getAsLong();
            if (result instanceof ZLinkOwnerLeaseFound found) {
                ObservedLease refreshed = new ObservedLease(
                    found.token(),
                    Duration.between(
                        found.storeNow(),
                        found.leaseExpiresAt()),
                    fetchedAt);
                observed.put(ownerId, refreshed);
                Duration remaining = refreshed.remaining(fetchedAt);
                return refreshed.matches(expectedGeneration)
                    && remaining.compareTo(Duration.ZERO) > 0
                        ? remaining
                        : null;
            }
            observed.remove(ownerId);
            return null;
        });
    }

    private static Duration requirePositive(
        Duration value,
        String name) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(
                name + " must be positive.");
        }
        return value;
    }

    private record ObservedLease(
        ZLinkLocationOwnerToken token,
        Duration storeRemaining,
        long fetchedAtNanos) {
        private boolean matches(long expectedGeneration) {
            return expectedGeneration == 0L
                || token.leaseGeneration() == expectedGeneration;
        }

        private Duration remaining(long nowNanos) {
            return storeRemaining.minus(
                Duration.ofNanos(nowNanos - fetchedAtNanos));
        }
    }
}
