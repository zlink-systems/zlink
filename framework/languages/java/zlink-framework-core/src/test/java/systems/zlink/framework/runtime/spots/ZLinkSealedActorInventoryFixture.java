package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.util.List;
import java.util.UUID;

final class ZLinkSealedActorInventoryFixture {
    private ZLinkSealedActorInventoryFixture() {}

    static void seal(
            ZLinkAggregateRelocationCoordinator coordinator,
            UUID relocationId,
            String authorityKey,
            ZLinkAuthoritySnapshot snapshot,
            byte[] targetAuthority,
            ZLinkStoreCancellation cancellation) {
        coordinator
                .prepareSource(
                        List.of(
                                new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                        authorityKey,
                                        snapshot.objectGeneration(),
                                        snapshot.authorityOwnerGeneration())),
                        authorityKey,
                        ZLinkAggregateFence.markerlessRelocation(relocationId),
                        1,
                        ignored -> targetAuthority,
                        cancellation)
                .toCompletableFuture()
                .join();
    }
}
