package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.HexFormat;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

/** Direct-transfer aggregate publication: authority records contain fences only. */
final class ZLinkAggregateRelocationCoordinatorTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;

    @Test
    void commitsFenceWithoutStagingPayloadAndNormalizesAfterCutover() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(authority);
        var request = request();

        var prepared = coordinator.prepare(request, NEVER).toCompletableFuture().join();
        assertEquals(1, authority.prepareCount);
        var publication = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authority.prepared.participants().getFirst().authorityPayload());
        assertNotNull(publication);
        var stagedOwner = new ZLinkServiceAuthorityPayloadCodec()
            .decode(publication.applicationPayload()).orElseThrow();
        assertEquals("owner-b", stagedOwner.ownerId());
        assertEquals(12, stagedOwner.ownerLeaseGeneration());

        var published = coordinator.commit(prepared, NEVER).toCompletableFuture().join();
        assertEquals(prepared.fence(), published.fence());
        assertEquals(1, authority.commitCount);

        coordinator.normalizePublishedAggregate(
            request.participants().stream().map(value ->
                new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                    value.authorityKey(), value.objectGeneration(),
                    value.authorityOwnerGeneration())).toList(),
            published.fence(), request.targetOwner(), NEVER).toCompletableFuture().join();

        var normalized = authority.rows.get("spot:room-a");
        assertEquals(null,
            ZLinkCanonicalRelocationAuthorityStateCodec.decode(normalized.payload()));
        var payload = new ZLinkServiceAuthorityPayloadCodec()
            .decode(normalized.payload()).orElseThrow();
        assertEquals("owner-b", payload.ownerId());
        assertEquals(12, payload.ownerLeaseGeneration());
        assertFalse(authority.progress.isPresent());
    }

    @Test
    void abortsOnlyThePreparedAuthorityFence() {
        FakeAuthorityStore authority = new FakeAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(authority);
        var prepared = coordinator.prepare(request(), NEVER).toCompletableFuture().join();

        coordinator.abort(prepared).toCompletableFuture().join();

        assertEquals(1, authority.abortCount);
        assertEquals(0, authority.commitCount);
    }

    private static ZLinkAggregateRelocationCoordinator.Request request() {
        byte[] payload = new ZLinkServiceAuthorityPayloadCodec().encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "RoomSpot", "room-a", "owner-a", 6,
            "game", RoutingId.from("node-a"), 3);
        return new ZLinkAggregateRelocationCoordinator.Request(
            new UUID(0, 9), 7, 11,
            java.util.List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                "spot:room-a", ZLinkPlacementObjectKind.USER_SPOT,
                3, 5, "version-1",
                ZLinkAuthorityGenerationTransition.NEW_OWNER, payload, new byte[0])),
            goldenRoot(),
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            4, ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT, "RoomSpot", 1),
            new ZLinkLocationOwnerToken("owner-b", 12),
            "version-1");
    }

    private static byte[] goldenRoot() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(fixture)) {
                try {
                    String json = Files.readString(fixture);
                    int start = json.indexOf("\"logicalHex\": \"");
                    if (start >= 0) {
                        start += "\"logicalHex\": \"".length();
                        return HexFormat.of().parseHex(json.substring(
                            start, json.indexOf('"', start)));
                    }
                } catch (IOException failure) {
                    throw new IllegalStateException(failure);
                }
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared relocation fixture was not found");
    }

    private static final class FakeAuthorityStore extends ZLinkLocationStoreTestAdapter {
        private ZLinkAggregatePrepareRequest prepared;
        private Optional<ZLinkAggregateProgressSnapshot> progress = Optional.empty();
        private int prepareCount;
        private int commitCount;
        private int abortCount;
        private final Map<String, ZLinkAuthoritySnapshot> rows =
            new java.util.concurrent.ConcurrentHashMap<>();

        @Override
        public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
            prepareCount++;
            prepared = request;
            return CompletableFuture.completedFuture(new ZLinkAggregatePrepared(
                new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration())));
        }

        @Override
        public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
            commitCount++;
            for (var participant : prepared.participants()) {
                rows.put(participant.authorityKey(), new ZLinkAuthoritySnapshot(
                    "committed-" + participant.authorityKey(),
                    participant.authorityPayload(), participant.objectGeneration(),
                    participant.sourceAuthorityOwnerGeneration() + 1,
                    prepared.targetOwner().ownerId(), prepared.targetOwner().leaseGeneration(),
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.USER_SPOT, "RoomSpot",
                        prepared.targetDescriptor(),
                        prepared.targetDescriptorLifecycleGeneration(),
                        prepared.capacityBundle()),
                    Instant.now()));
            }
            progress = Optional.of(new ZLinkAggregateProgressSnapshot(
                fence, "aggregate-commit-" + commitCount, prepared));
            return CompletableFuture.completedFuture(ZLinkAggregateCommitResult.COMMITTED);
        }

        @Override
        public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(progress);
        }

        @Override
        public CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence, String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
            if (progress.isPresent()
                && progress.get().storeVersion().equals(expectedStoreVersion)) {
                progress = Optional.empty();
                return CompletableFuture.completedFuture(true);
            }
            return CompletableFuture.completedFuture(false);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
            String key, ZLinkStoreCancellation cancellation) {
            var current = rows.get(key);
            return CompletableFuture.completedFuture(current == null
                ? new ZLinkAuthorityMissing(Instant.now()) : current);
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key, ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation, ZLinkStoreCancellation cancellation) {
            var current = rows.get(key);
            if (!(expectation instanceof ZLinkAuthorityExpectFound found)
                || !(mutation instanceof ZLinkAuthorityPut put)
                || current == null || !current.storeVersion().equals(found.storeVersion())) {
                return CompletableFuture.completedFuture(new ZLinkAuthorityConflict(
                    current == null ? new ZLinkAuthorityMissing(Instant.now()) : current));
            }
            var stored = new ZLinkAuthoritySnapshot(
                "normalized-" + key, put.payload(), current.objectGeneration(),
                current.authorityOwnerGeneration(), current.ownerId(),
                current.ownerLeaseGeneration(), current.allocation(), Instant.now());
            rows.put(key, stored);
            return CompletableFuture.completedFuture(new ZLinkAuthorityStored(
                stored.storeVersion(), stored.payload(), stored.objectGeneration(),
                stored.authorityOwnerGeneration(), stored.ownerId(),
                stored.ownerLeaseGeneration(), stored.allocation(), stored.storeNow()));
        }

        @Override
        public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
            abortCount++;
            return CompletableFuture.completedFuture(ZLinkAggregateAbortResult.ABORTED);
        }
    }
}
