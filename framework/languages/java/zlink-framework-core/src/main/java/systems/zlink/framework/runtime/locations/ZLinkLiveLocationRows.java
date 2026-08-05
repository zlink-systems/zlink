package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.ZLinkLocationOptions;

/** Applies owner-lease admission to authority-derived runtime projections. */
public final class ZLinkLiveLocationRows {
    private final ZLinkOwnerLeaseTracker leaseTracker;
    private final Duration routeCacheMaxAge;

    public static ZLinkLiveLocationRows create(
        ZLinkRegisteredLocationStores stores,
        ZLinkLocationOptions options) {
        Objects.requireNonNull(stores, "stores");
        Objects.requireNonNull(options, "options");
        return new ZLinkLiveLocationRows(
            new ZLinkOwnerLeaseTracker(
                stores.unifiedStore(), options.pollingInterval()),
            options.routeCacheMaxAge());
    }

    ZLinkLiveLocationRows(
        ZLinkOwnerLeaseTracker leaseTracker,
        Duration routeCacheMaxAge) {
        this.leaseTracker = Objects.requireNonNull(leaseTracker, "leaseTracker");
        this.routeCacheMaxAge = Objects.requireNonNull(routeCacheMaxAge, "routeCacheMaxAge");
    }

    CompletionStage<Duration> ownerLeaseRemaining(
        String ownerId,
        long ownerLeaseGeneration) {
        return leaseTracker.remainingAdmissionLifetime(ownerId, ownerLeaseGeneration);
    }

    Duration routeCacheMaxAge() {
        return routeCacheMaxAge;
    }
}
