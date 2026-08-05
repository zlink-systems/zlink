package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Duration;

public final class ZLinkLocationOptions {
    private Duration ownerLeaseRenewInterval = Duration.ofSeconds(5);
    private Duration ownerLeaseTtl = Duration.ofSeconds(15);
    private Duration pollingInterval = Duration.ofSeconds(1);
    private Duration storeFailureGrace = Duration.ofSeconds(30);
    private Duration ownerLeaseFencingMargin = Duration.ofSeconds(5);
    private Duration ownerLeaseRenewTimeout = Duration.ofSeconds(3);
    private Duration routeCacheMaxAge = Duration.ofSeconds(15);
    private Duration messageFollowDuration = Duration.ofSeconds(30);
    private int maxActiveOutboundRelocations = 64;
    private int maxActiveInboundRelocations = 64;
    private int maxConcurrentRelocationCaptures = 8;
    private int maxConcurrentRelocationRestores = 8;
    private long maxRelocationPayloadInFlightBytes = 256L * 1024 * 1024;

    public Duration ownerLeaseRenewInterval() {
        return ownerLeaseRenewInterval;
    }

    public void setOwnerLeaseRenewInterval(Duration value) {
        ownerLeaseRenewInterval = requirePositive(
            value,
            "ownerLeaseRenewInterval");
    }

    public Duration ownerLeaseTtl() {
        return ownerLeaseTtl;
    }

    public void setOwnerLeaseTtl(Duration ownerLeaseTtl) {
        this.ownerLeaseTtl = requirePositive(ownerLeaseTtl, "ownerLeaseTtl");
    }

    public Duration pollingInterval() {
        return pollingInterval;
    }

    public void setPollingInterval(Duration pollingInterval) {
        this.pollingInterval = requirePositive(pollingInterval, "pollingInterval");
    }

    public Duration storeFailureGrace() {
        return storeFailureGrace;
    }

    public void setStoreFailureGrace(Duration storeFailureGrace) {
        this.storeFailureGrace = requirePositive(storeFailureGrace, "storeFailureGrace");
    }

    public Duration ownerLeaseFencingMargin() {
        return ownerLeaseFencingMargin;
    }

    public void setOwnerLeaseFencingMargin(Duration value) {
        ownerLeaseFencingMargin = requirePositive(
            value,
            "ownerLeaseFencingMargin");
    }

    public Duration ownerLeaseRenewTimeout() {
        return ownerLeaseRenewTimeout;
    }

    public void setOwnerLeaseRenewTimeout(Duration ownerLeaseRenewTimeout) {
        this.ownerLeaseRenewTimeout = requirePositive(
            ownerLeaseRenewTimeout,
            "ownerLeaseRenewTimeout");
    }

    public Duration routeCacheMaxAge() {
        return routeCacheMaxAge;
    }

    public void setRouteCacheMaxAge(Duration routeCacheMaxAge) {
        Duration candidate = requireNonNegative(
            routeCacheMaxAge,
            "routeCacheMaxAge");
        validateRouteLifetimeRelationship(
            candidate,
            messageFollowDuration);
        this.routeCacheMaxAge = candidate;
    }

    public Duration messageFollowDuration() {
        return messageFollowDuration;
    }

    public void setMessageFollowDuration(
        Duration messageFollowDuration) {
        Duration candidate = requireNonNegative(
            messageFollowDuration,
            "messageFollowDuration");
        validateRouteLifetimeRelationship(routeCacheMaxAge, candidate);
        this.messageFollowDuration = candidate;
    }

    public int maxActiveOutboundRelocations() {
        return maxActiveOutboundRelocations;
    }

    public void setMaxActiveOutboundRelocations(int value) {
        maxActiveOutboundRelocations = requirePositive(
            value,
            "maxActiveOutboundRelocations");
    }

    public int maxActiveInboundRelocations() {
        return maxActiveInboundRelocations;
    }

    public void setMaxActiveInboundRelocations(int value) {
        maxActiveInboundRelocations = requirePositive(
            value,
            "maxActiveInboundRelocations");
    }

    public int maxConcurrentRelocationCaptures() {
        return maxConcurrentRelocationCaptures;
    }

    public void setMaxConcurrentRelocationCaptures(int value) {
        maxConcurrentRelocationCaptures = requirePositive(
            value,
            "maxConcurrentRelocationCaptures");
    }

    public int maxConcurrentRelocationRestores() {
        return maxConcurrentRelocationRestores;
    }

    public void setMaxConcurrentRelocationRestores(int value) {
        maxConcurrentRelocationRestores = requirePositive(
            value,
            "maxConcurrentRelocationRestores");
    }

    public long maxRelocationPayloadInFlightBytes() {
        return maxRelocationPayloadInFlightBytes;
    }

    public void setMaxRelocationPayloadInFlightBytes(long value) {
        if (value <= 0) {
            throw new IllegalArgumentException(
                "maxRelocationPayloadInFlightBytes must be positive.");
        }
        maxRelocationPayloadInFlightBytes = value;
    }

    private static Duration requirePositive(Duration value, String name) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(name + " must be positive.");
        }
        return value;
    }

    private static int requirePositive(int value, String name) {
        if (value <= 0) {
            throw new IllegalArgumentException(name + " must be positive.");
        }
        return value;
    }

    private static Duration requireNonNegative(Duration value, String name) {
        if (value == null || value.isNegative()) {
            throw new IllegalArgumentException(
                name + " must be greater than or equal to zero.");
        }
        return value;
    }

    private static void validateRouteLifetimeRelationship(
        Duration routeCacheMaxAge,
        Duration messageFollowDuration) {
        if (!routeCacheMaxAge.isZero()
            && !messageFollowDuration.isZero()
            && routeCacheMaxAge.compareTo(
                messageFollowDuration.minusSeconds(5)) > 0) {
            throw new IllegalArgumentException(
                "routeCacheMaxAge must be at least five seconds shorter than "
                    + "messageFollowDuration when both values are enabled.");
        }
    }

}
