package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;

import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class ZLinkCandidateLeaseFixture {
    private ZLinkCandidateLeaseFixture() {}

    public static ZLinkLiveLocationRows liveRows(
            ZLinkLocationRepository store, java.util.function.LongSupplier ticker) {
        return new ZLinkLiveLocationRows(
                new ZLinkOwnerLeaseTracker(store, java.time.Duration.ofMillis(10), ticker),
                java.time.Duration.ofSeconds(30));
    }

    public enum Lease {
        MISSING,
        EXPIRED,
        REPLACED,
        WRONG_OWNER,
        LIVE,
        CORRUPT,
        FAILED
    }

    public static final IllegalStateException READ_FAILURE =
            new IllegalStateException("owner read failed");
    public static final Instant STORE_NOW = Instant.parse("2040-01-01T00:00:00Z");

    public static CompletionStage<ZLinkOwnerLeaseReadResult> read(
            Lease lease, String owner, long generation) {
        if (lease == Lease.MISSING)
            return CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
        if (lease == Lease.FAILED) return CompletableFuture.failedFuture(READ_FAILURE);
        return CompletableFuture.completedFuture(
                new ZLinkOwnerLeaseFound(
                        new ZLinkLocationOwnerToken(
                                lease == Lease.WRONG_OWNER ? "other-owner" : owner,
                                lease == Lease.REPLACED ? generation + 1 : generation),
                        lease == Lease.CORRUPT
                                ? null
                                : lease == Lease.EXPIRED ? STORE_NOW : STORE_NOW.plusSeconds(30),
                        STORE_NOW));
    }
}
