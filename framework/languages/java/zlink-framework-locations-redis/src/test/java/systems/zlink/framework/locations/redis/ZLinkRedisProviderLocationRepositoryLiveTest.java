package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDeleted;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

/**
 * Live-Redis behavioral oracle for the production Location Store path
 * ({@link ZLinkRedisLocationStore} wrapped by
 * {@code ZLinkProviderLocationRepository}), independent of the dead
 * dedicated-Lua/HASH stack (the now-deleted {@code ZLinkRedisLocationRepository}
 * and friends).
 *
 * <p>Before this test existed, live-Redis behavioral coverage of lease
 * claim/renew/release/expiry, descriptor publish/list/takeover fencing,
 * authority reserve/commit/CAS, and capacity admission only ran against the
 * legacy Lua/HASH stack, which the live production wiring
 * ({@link ZLinkRedisLocationStore} constructed directly by every sample and
 * e2e app) never uses. This class is that missing oracle, and is a
 * prerequisite for safely deleting the legacy stack.</p>
 */
final class ZLinkRedisProviderLocationRepositoryLiveTest {
    private static final Instant UPDATED_AT =
        Instant.parse("2026-07-03T00:00:00Z");
    private static final RoutingId NODE_A =
        RoutingId.from(new byte[] {0x01});

    @Test
    void ownerLeaseClaimRenewReleaseAndExpiryAreFenced() throws Exception {
        try (var handle = liveRepository("owner-lease-live")) {
            ZLinkLocationRepository store = handle.repository();
            var claimed = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "owner-live-a", Duration.ofSeconds(30))
                    .toCompletableFuture().get());
            var token = claimed.token();

            var found = assertInstanceOf(
                ZLinkOwnerLeaseFound.class,
                store.readOwnerLease("owner-live-a")
                    .toCompletableFuture().get());
            assertEquals(token, found.token());

            assertInstanceOf(
                ZLinkOwnerLeaseRenewed.class,
                store.renewOwnerLease(token, Duration.ofSeconds(30))
                    .toCompletableFuture().get());

            var staleToken = new ZLinkLocationOwnerToken(
                token.ownerId(), token.leaseGeneration() + 1);
            assertInstanceOf(
                ZLinkOwnerLeaseRenewStale.class,
                store.renewOwnerLease(staleToken, Duration.ofSeconds(30))
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkOwnerLeaseReleaseResult.STALE,
                store.releaseOwnerLease(staleToken)
                    .toCompletableFuture().get());

            assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                store.releaseOwnerLease(token)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkOwnerLeaseMissing.class,
                store.readOwnerLease("owner-live-a")
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkOwnerLeaseReleaseResult.STALE,
                store.releaseOwnerLease(token)
                    .toCompletableFuture().get());

            var shortLived = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "owner-live-a", Duration.ofMillis(300))
                    .toCompletableFuture().get());
            assertEquals(
                token.leaseGeneration() + 1,
                shortLived.token().leaseGeneration(),
                "generation must remain monotonic across release+reclaim");
            Thread.sleep(1500);
            assertInstanceOf(
                ZLinkOwnerLeaseMissing.class,
                store.readOwnerLease("owner-live-a")
                    .toCompletableFuture().get());
        }
    }

    @Test
    void meshNodeDescriptorPublishListTakeoverAndCapacityAreFenced()
        throws Exception {
        try (var handle = liveRepository("mesh-descriptor-live")) {
            ZLinkLocationRepository store = handle.repository();
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "descriptor-owner-live", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();

            var initial = descriptor(NODE_A, 11, 1, owner, "player", 2, 1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        initial, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            // A renew must strictly advance descriptorRevision -- replaying
            // the exact same revision is not idempotent, it is stale.
            var renewed = descriptor(NODE_A, 11, 2, owner, "player", 2, 1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(renewed, ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());

            var page = store.listMeshNodes(
                    "game", ZLinkPageRequest.firstPage())
                .toCompletableFuture().get();
            var stored = page.items().getFirst();
            assertEquals(renewed.meshName(), stored.meshName());
            assertEquals(
                renewed.objectCapabilities(), stored.objectCapabilities());
            assertEquals(renewed.capacity(), stored.capacity());

            // Same descriptorRevision as the currently stored row is not a
            // valid renew, and a live owner lease blocks a takeover: the
            // write must be ignored, not silently overwrite state.
            var sameRevisionDifferentBytes =
                descriptor(NODE_A, 11, 2, owner, "different-type", 2, 1);
            assertEquals(
                ZLinkLocationWriteStatus.IGNORED_STALE,
                store.updateMeshNode(
                        sameRevisionDifferentBytes,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());

            // A different owner cannot take over while the current lease
            // is still live.
            var otherOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "descriptor-owner-live-2", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var takeoverAttempt =
                descriptor(NODE_A, 11, 3, otherOwner, "player", 2, 1);
            assertEquals(
                ZLinkLocationWriteStatus.IGNORED_STALE,
                store.updateMeshNode(
                        takeoverAttempt, ZLinkLocationWriteIntent.TAKEOVER)
                    .toCompletableFuture().get().status());

            var mutableUpdate =
                descriptor(NODE_A, 11, 3, owner, "player", 2, 1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        mutableUpdate, ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());

            String firstKey = ZLinkAuthorityKeyCodec.actor("descriptor-capacity-live-1");
            var first = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            firstKey, mutableUpdate, owner, "player"),
                        () -> false)
                    .toCompletableFuture().get());
            var second = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            ZLinkAuthorityKeyCodec.actor("descriptor-capacity-live-2"),
                            mutableUpdate, owner, "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                store.reserve(
                        reservationRequest(
                            ZLinkAuthorityKeyCodec.actor("descriptor-capacity-live-3"),
                            mutableUpdate, owner, "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectAbortResult.ABORTED,
                store.abort(second.reservation(), () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkObjectConflict.class,
                store.reserve(
                        reservationRequest(
                            ZLinkAuthorityKeyCodec.actor("descriptor-unsupported-type-live"),
                            mutableUpdate, owner, "unsupported"),
                        () -> false)
                    .toCompletableFuture().get());

            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey("game", NODE_A),
                        owner)
                    .toCompletableFuture().get());
            var replacement =
                descriptor(NODE_A, 12, 1, owner, "player", 3, 1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        replacement, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            // NOTE: unlike the legacy Lua/HASH path, ZLinkProviderAuthorityRepository's
            // commit() fences only against the authority row's own identity,
            // not against the target mesh node's current lifecycleGeneration --
            // a reservation captured against a since-replaced descriptor still
            // commits. Documented here as observed current behavior, not
            // necessarily the intended contract; flagged separately.
            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(first.reservation(), new byte[] {2}, () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectAbortResult.STALE,
                store.abort(first.reservation(), () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                1L,
                store.removeAllByOwner(owner).toCompletableFuture().get());
            // removeAllByOwner only reclaims authority rows (reservations
            // and active allocations); mesh node descriptors are a
            // separate lifecycle removed only through removeMeshNode.
            assertEquals(
                1,
                store.listMeshNodes(
                        "game", ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get().items().size());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey("game", NODE_A),
                        owner)
                    .toCompletableFuture().get());
            assertEquals(
                List.of(),
                store.listMeshNodes(
                        "game", ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get().items());
        }
    }

    @Test
    void authorityReserveCommitAndCompareExchangeAreFenced()
        throws Exception {
        try (var handle = liveRepository("authority-cas-live")) {
            ZLinkLocationRepository store = handle.repository();
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "authority-owner-live", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var descriptor =
                descriptor(NODE_A, 1, 1, owner, "player", 1, 0);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());

            var first = reservationRequest(
                ZLinkAuthorityKeyCodec.actor("authority-cas-live-1"), descriptor, owner, "player");
            var second = reservationRequest(
                ZLinkAuthorityKeyCodec.actor("authority-cas-live-2"), descriptor, owner, "player");

            var reservation = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(first, () -> false)
                    .toCompletableFuture().get()).reservation();
            assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                store.reserve(second, () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(
                        reservation, new byte[] {2}, () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                store.reserve(second, () -> false)
                    .toCompletableFuture().get());

            var current = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(first.authorityKey(), () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                store.compareExchange(
                        first.authorityKey(),
                        new ZLinkAuthorityExpectFound(
                            current.storeVersion()),
                        new ZLinkAuthorityDelete(),
                        () -> false)
                    .toCompletableFuture().get());
            // Now that the row is gone, the freed capacity admits a new
            // reservation.
            assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(second, () -> false)
                    .toCompletableFuture().get());
        }
    }

    private static LiveRepository liveRepository(String label) {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        ZLinkRedisLocationStore store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix(
                    "zlink:" + label + "-test:" + UUID.randomUUID()));
        return new LiveRepository(
            store, new ZLinkProviderLocationRepository(store));
    }

    private record LiveRepository(
        ZLinkRedisLocationStore store,
        ZLinkLocationRepository repository) implements AutoCloseable {
        @Override
        public void close() {
            store.close();
        }
    }

    private static ZLinkObjectReservationRequest reservationRequest(
        String authorityKey,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner,
        String stableType) {
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.ACTOR,
            authorityKey,
            stableType,
            "creation-root",
            new byte[32],
            32,
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(), descriptor.rid()),
            descriptor.lifecycleGeneration(),
            owner,
            new byte[] {1},
            ZLinkPlacementCapacityBundle.actor(1));
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long lifecycleGeneration,
        long descriptorRevision,
        ZLinkLocationOwnerToken owner,
        String stableType,
        int activeLimit,
        int pendingLimit) {
        return new ZLinkMeshNodeDescriptor(
            "game",
            rid,
            lifecycleGeneration,
            descriptorRevision,
            "tcp://127.0.0.1:7000",
            Map.of("game", 100),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                stableType,
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry-" + rid),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, activeLimit),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            UPDATED_AT);
    }
}
