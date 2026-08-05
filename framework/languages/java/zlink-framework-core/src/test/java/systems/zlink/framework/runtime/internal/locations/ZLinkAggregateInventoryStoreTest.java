package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

final class ZLinkAggregateInventoryStoreTest {
    @Test
    void storesAndReadsInventoryAcrossTheLeafPageBound() {
        List<ZLinkAggregateParticipant> participants = new ArrayList<>();
        for (int index = 0; index < 2_050; index++) {
            participants.add(new ZLinkAggregateParticipant(
                String.format("authority:%04d", index),
                index + 1L,
                index + 2L,
                "version-" + index,
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                new byte[] {(byte) index, 1, 2},
                new byte[] {(byte) index, 3}));
        }
        var request = new ZLinkAggregatePrepareRequest(
            UUID.randomUUID(),
            1,
            participants,
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            1,
            ZLinkPlacementCapacityBundle.actor(2_050),
            new ZLinkLocationOwnerToken("owner-b", 1));
        var fence = new ZLinkAggregateFence(
            request.aggregateId(),
            request.aggregateGeneration());
        var store = new ZLinkAggregateInventoryStore(
            new ZLinkInMemoryProviderLocationStore());

        store.store(request, () -> false).toCompletableFuture().join();
        List<ZLinkAggregateParticipant> loaded = store.load(
                fence,
                participants.size(),
                request.inventoryDigest(),
                () -> false)
            .toCompletableFuture()
            .join();

        assertEquals(participants.size(), loaded.size());
        for (int index = 0; index < participants.size(); index++) {
            assertEquals(
                participants.get(index).authorityKey(),
                loaded.get(index).authorityKey());
            assertEquals(
                participants.get(index).expectedStoreVersion(),
                loaded.get(index).expectedStoreVersion());
            assertArrayEquals(
                participants.get(index).authorityPayload(),
                loaded.get(index).authorityPayload());
            assertArrayEquals(
                participants.get(index).membershipMutation(),
                loaded.get(index).membershipMutation());
        }
    }

    @Test
    void deletesAllInventoryValuesAcrossMultipleProviderPages() {
        List<ZLinkAggregateParticipant> participants = new ArrayList<>();
        for (int index = 0; index < 1_001; index++) {
            participants.add(new ZLinkAggregateParticipant(
                "authority:%04d".formatted(index),
                index + 1L,
                index + 2L,
                "version-" + index,
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                new byte[] {(byte) index, 1},
                new byte[] {(byte) index, 2}));
        }
        var request = new ZLinkAggregatePrepareRequest(
            UUID.randomUUID(),
            2,
            participants,
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            1,
            ZLinkPlacementCapacityBundle.actor(1_001),
            new ZLinkLocationOwnerToken("owner-b", 1));
        var fence = new ZLinkAggregateFence(
            request.aggregateId(),
            request.aggregateGeneration());
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkAggregateInventoryStore(provider);

        store.store(request, () -> false).toCompletableFuture().join();
        store.delete(fence, () -> false).toCompletableFuture().join();
        store.delete(fence, () -> false).toCompletableFuture().join();

        var remaining = (ZLinkStoreScanPageResult) provider.scan(
                new ZLinkStoreScanRequest(
                    "zlink:v11:aggregate-inventory:" + fence.aggregateId()
                        + ":" + fence.aggregateGeneration() + ":",
                    null,
                    1_000),
                () -> false)
            .toCompletableFuture()
            .join();
        assertTrue(remaining.value().items().isEmpty());
    }
}
