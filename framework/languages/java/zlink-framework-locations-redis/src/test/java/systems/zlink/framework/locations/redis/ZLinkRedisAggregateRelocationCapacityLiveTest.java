package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateParticipant;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepared;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDeleted;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;

/**
 * Live-Redis oracle for host relocation capacity (Location runtime §6.1 {@code NewOwner}, #1979):
 * an aggregate prepare reserves the target capacity, commit turns that reservation into active
 * usage, and abort returns it exactly once.
 */
final class ZLinkRedisAggregateRelocationCapacityLiveTest {
    private static final Instant UPDATED_AT = Instant.parse("2026-10-16T00:00:00Z");
    private static final RoutingId SOURCE = RoutingId.from("relocation-capacity-source");
    private static final RoutingId TARGET = RoutingId.from("relocation-capacity-target");

    @Test
    void relocatedSpotCountsAgainstTargetCapacityFromPrepare() throws Exception {
        try (var fixture = Fixture.open("relocation-capacity-counts")) {
            String moved = fixture.activeRoom("moved-room", fixture.source);

            var prepared = fixture.prepare(moved);
            // The prepared relocation already holds the target's only room slot.
            fixture.assertExhausted(fixture.target, "target-during-prepare");

            assertEquals(
                    ZLinkAggregateCommitResult.COMMITTED,
                    fixture.repository
                            .commitAggregate(prepared.fence(), () -> false)
                            .toCompletableFuture()
                            .get());
            assertEquals(TARGET, fixture.snapshot(moved).allocation().descriptor().rid());
            fixture.assertExhausted(fixture.target, "target-after-commit");
            // The source slot is released by the same commit.
            fixture.reserve(fixture.source, "source-after-commit");
        }
    }

    @Test
    void fullTargetRefusesRelocationAndNewSpot() throws Exception {
        try (var fixture = Fixture.open("relocation-capacity-full")) {
            String resident = fixture.activeRoom("resident-room", fixture.target);
            String moved = fixture.activeRoom("moved-room", fixture.source);
            ZLinkAuthoritySnapshot before = fixture.snapshot(moved);

            assertInstanceOf(
                    ZLinkAggregateConflict.class,
                    fixture.repository
                            .prepareAggregate(fixture.request(moved), () -> false)
                            .toCompletableFuture()
                            .get());
            fixture.assertExhausted(fixture.target, "target-full");
            assertEquals(before.storeVersion(), fixture.snapshot(moved).storeVersion());

            // The refused prepare left no participant marker or reservation behind: once the
            // resident leaves, a new relocation of the same Spot is admitted.
            ZLinkAuthoritySnapshot residentSnapshot = fixture.snapshot(resident);
            assertInstanceOf(
                    ZLinkAuthorityDeleted.class,
                    fixture.repository
                            .compareExchange(
                                    resident,
                                    new ZLinkAuthorityExpectFound(residentSnapshot.storeVersion()),
                                    new ZLinkAuthorityDelete(),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            fixture.prepare(moved);
            fixture.assertExhausted(fixture.target, "target-after-second-prepare");
        }
    }

    @Test
    void abortReleasesTargetReservationExactlyOnce() throws Exception {
        try (var fixture = Fixture.open("relocation-capacity-abort")) {
            String moved = fixture.activeRoom("moved-room", fixture.source);
            ZLinkAuthoritySnapshot before = fixture.snapshot(moved);

            var prepared = fixture.prepare(moved);
            fixture.assertExhausted(fixture.target, "target-during-prepare");

            assertEquals(
                    ZLinkAggregateAbortResult.ABORTED,
                    fixture.repository
                            .abortAggregate(prepared.fence(), () -> false)
                            .toCompletableFuture()
                            .get());
            assertEquals(SOURCE, fixture.snapshot(moved).allocation().descriptor().rid());
            assertEquals(
                    before.authorityOwnerGeneration(),
                    fixture.snapshot(moved).authorityOwnerGeneration());
            // The released slot admits exactly one new Spot.
            fixture.reserve(fixture.target, "target-after-abort");
            assertEquals(
                    ZLinkAggregateAbortResult.ALREADY_ABORTED,
                    fixture.repository
                            .abortAggregate(prepared.fence(), () -> false)
                            .toCompletableFuture()
                            .get());
            // A repeated abort must not release the slot a second time.
            fixture.assertExhausted(fixture.target, "target-after-repeated-abort");
        }
    }

    private static final class Fixture implements AutoCloseable {
        private final ZLinkRedisLocationStore store;
        private final ZLinkLocationRepository repository;
        private final ZLinkLocationOwnerToken owner;
        private final ZLinkMeshNodeDescriptor source;
        private final ZLinkMeshNodeDescriptor target;

        private Fixture(
                ZLinkRedisLocationStore store,
                ZLinkLocationRepository repository,
                ZLinkLocationOwnerToken owner,
                ZLinkMeshNodeDescriptor source,
                ZLinkMeshNodeDescriptor target) {
            this.store = store;
            this.repository = repository;
            this.owner = owner;
            this.source = source;
            this.target = target;
        }

        static Fixture open(String label) throws Exception {
            String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
            assumeTrue(
                    endpoint != null && !endpoint.isBlank(),
                    "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
            var store =
                    new ZLinkRedisLocationStore(
                            new ZLinkRedisLocationOptions()
                                    .setConnectionString(endpoint)
                                    .setKeyPrefix("zlink:" + label + "-test:" + UUID.randomUUID()));
            var repository = new ZLinkProviderLocationRepository(store);
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease(
                                                    label + "-owner", Duration.ofMinutes(5))
                                            .toCompletableFuture()
                                            .get())
                            .token();
            // The source has no room limit; the target admits exactly one room.
            var source = descriptor(SOURCE, owner, 0);
            var target = descriptor(TARGET, owner, 1);
            for (var node : List.of(source, target)) {
                assertEquals(
                        ZLinkLocationWriteStatus.STORED,
                        repository
                                .updateMeshNode(node, ZLinkLocationWriteIntent.NEW_CLAIM)
                                .toCompletableFuture()
                                .get()
                                .status());
            }
            return new Fixture(store, repository, owner, source, target);
        }

        String activeRoom(String spotId, ZLinkMeshNodeDescriptor node) throws Exception {
            var reserved = reserve(node, spotId);
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    repository
                            .commit(reserved.reservation(), new byte[] {1}, () -> false)
                            .toCompletableFuture()
                            .get());
            return ZLinkAuthorityKeyCodec.spot(spotId);
        }

        ZLinkObjectReserved reserve(ZLinkMeshNodeDescriptor node, String spotId) throws Exception {
            return assertInstanceOf(
                    ZLinkObjectReserved.class,
                    repository
                            .reserve(roomRequest(node, spotId), () -> false)
                            .toCompletableFuture()
                            .get());
        }

        void assertExhausted(ZLinkMeshNodeDescriptor node, String spotId) throws Exception {
            assertInstanceOf(
                    ZLinkPlacementCapacityExhausted.class,
                    repository
                            .reserve(roomRequest(node, spotId), () -> false)
                            .toCompletableFuture()
                            .get());
        }

        ZLinkAuthoritySnapshot snapshot(String key) throws Exception {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    repository.read(key, () -> false).toCompletableFuture().get());
        }

        ZLinkAggregatePrepared prepare(String key) throws Exception {
            return assertInstanceOf(
                    ZLinkAggregatePrepared.class,
                    repository
                            .prepareAggregate(request(key), () -> false)
                            .toCompletableFuture()
                            .get());
        }

        ZLinkAggregatePrepareRequest request(String key) throws Exception {
            ZLinkAuthoritySnapshot current = snapshot(key);
            return new ZLinkAggregatePrepareRequest(
                    UUID.randomUUID(),
                    1,
                    List.of(
                            new ZLinkAggregateParticipant(
                                    key,
                                    current.objectGeneration(),
                                    current.authorityOwnerGeneration(),
                                    current.storeVersion(),
                                    ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                    new byte[] {2},
                                    new byte[0])),
                    new byte[32],
                    new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid()),
                    target.lifecycleGeneration(),
                    current.allocation().capacityBundle(),
                    owner);
        }

        private ZLinkObjectReservationRequest roomRequest(
                ZLinkMeshNodeDescriptor node, String spotId) {
            return new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    ZLinkAuthorityKeyCodec.spot(spotId),
                    "room",
                    "inline-v1:test",
                    new byte[32],
                    4,
                    new ZLinkMeshNodeDescriptorKey(node.meshName(), node.rid()),
                    node.lifecycleGeneration(),
                    owner,
                    new byte[] {1},
                    ZLinkPlacementCapacityBundle.spot(
                            ZLinkPlacementObjectKind.USER_SPOT, "room", 1));
        }

        @Override
        public void close() {
            store.close();
        }
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid, ZLinkLocationOwnerToken owner, int roomLimit) {
        return new ZLinkMeshNodeDescriptor(
                "game",
                rid,
                1,
                1,
                "tcp://127.0.0.1:7100",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                "room",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                roomLimit)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("entry-" + rid),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 64),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                UPDATED_AT);
    }
}
