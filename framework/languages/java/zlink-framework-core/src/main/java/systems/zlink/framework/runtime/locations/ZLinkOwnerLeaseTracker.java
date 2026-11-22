package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongSupplier;

final class ZLinkOwnerLeaseTracker {
    private final ZLinkLocationRepository store;
    private final Duration pollingInterval;
    private final LongSupplier nanoTime;
    private final Map<String, ObservedLease> observed = new ConcurrentHashMap<>();

    ZLinkOwnerLeaseTracker(ZLinkLocationRepository store, Duration pollingInterval) {
        this(store, pollingInterval, System::nanoTime);
    }

    ZLinkOwnerLeaseTracker(
            ZLinkLocationRepository store, Duration pollingInterval, LongSupplier nanoTime) {
        this.store = Objects.requireNonNull(store, "store");
        this.pollingInterval = requirePositive(pollingInterval, "pollingInterval");
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
    }

    CompletionStage<Boolean> isOwnerLive(String ownerId) {
        return remainingAdmissionLifetime(ownerId, 0L).thenApply(remaining -> remaining != null);
    }

    CompletionStage<Duration> remainingAdmissionLifetime(String ownerId, long expectedGeneration) {
        return admittedLease(ownerId, expectedGeneration, (remaining, sampledAtNanos) -> remaining);
    }

    CompletionStage<LeaseAdmission> leaseAdmission(String ownerId, long expectedGeneration) {
        return admittedLease(ownerId, expectedGeneration, LeaseAdmission::new);
    }

    private <T> CompletionStage<T> admittedLease(
            String ownerId, long expectedGeneration, LeaseProjection<T> projection) {
        ObservedLease current = observed.get(ownerId);
        long now = nanoTime.getAsLong();
        if (current != null
                && Duration.ofNanos(now - current.fetchedAtNanos).compareTo(pollingInterval) < 0) {
            return CompletableFuture.completedFuture(
                    current.admit(ownerId, expectedGeneration, now, projection));
        }
        return store.readOwnerLease(ownerId)
                .thenApply(
                        result -> {
                            if (result instanceof ZLinkOwnerLeaseFound found) {
                                long fetchedAt = nanoTime.getAsLong();
                                ObservedLease refreshed =
                                        new ObservedLease(
                                                found.token(),
                                                Duration.between(
                                                        found.storeNow(), found.leaseExpiresAt()),
                                                fetchedAt);
                                observed.put(ownerId, refreshed);
                                return refreshed.admit(
                                        ownerId, expectedGeneration, fetchedAt, projection);
                            }
                            observed.remove(ownerId);
                            return null;
                        });
    }

    record LeaseAdmission(Duration remaining, long sampledAtNanos) {}

    private interface LeaseProjection<T> {
        T project(Duration remaining, long sampledAtNanos);
    }

    boolean hasObservedLiveLease(String ownerId, long expectedGeneration) {
        ObservedLease current = observed.get(ownerId);
        return current != null
                && current.matches(ownerId, expectedGeneration)
                && current.remaining(nanoTime.getAsLong()).compareTo(Duration.ZERO) > 0;
    }

    private static Duration requirePositive(Duration value, String name) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(name + " must be positive.");
        }
        return value;
    }

    private record ObservedLease(
            ZLinkLocationOwnerToken token, Duration storeRemaining, long fetchedAtNanos) {
        private boolean matches(String ownerId, long expectedGeneration) {
            return token.ownerId().equals(ownerId)
                    && (expectedGeneration == 0L || token.leaseGeneration() == expectedGeneration);
        }

        private <T> T admit(
                String ownerId, long generation, long nowNanos, LeaseProjection<T> projection) {
            if (!matches(ownerId, generation)) return null;
            Duration remaining = remaining(nowNanos);
            return remaining.compareTo(Duration.ZERO) > 0
                    ? projection.project(remaining, nowNanos)
                    : null;
        }

        private Duration remaining(long nowNanos) {
            return storeRemaining.minus(Duration.ofNanos(nowNanos - fetchedAtNanos));
        }
    }
}
