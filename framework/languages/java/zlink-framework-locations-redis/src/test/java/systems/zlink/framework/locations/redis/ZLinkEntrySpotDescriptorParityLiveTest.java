package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkEntrySpotDescriptorParityLiveTest {
    private static final String ENTRY_A = "mesh-entry-00000000-0000-4000-8000-000000000001";
    private static final String ENTRY_B = "mesh-entry-00000000-0000-4000-8000-000000000002";

    @ParameterizedTest(name = "second Entry Spot ID {0}")
    @ValueSource(strings = {ENTRY_A, ENTRY_B})
    void newClaimReturnsSameStatusInMemoryAndRedis(String secondEntrySpotId) throws Exception {
        try (var redis = openRedis()) {
            var memoryStatus = publishSecond(new ZLinkInMemoryLocationStore(), secondEntrySpotId);
            var redisStatus =
                    publishSecond(new ZLinkProviderLocationRepository(redis), secondEntrySpotId);

            assertEquals(memoryStatus, redisStatus, "in-memory and Redis NewClaim status");
            assertEquals(
                    ENTRY_B.equals(secondEntrySpotId)
                            ? ZLinkLocationWriteStatus.STORED
                            : ZLinkLocationWriteStatus.REJECTED_CONFLICT,
                    redisStatus);
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void entryClaimIsReleasedOnlyByMatchingDescriptorOrOwnerCleanup(boolean ownerCleanup)
            throws Exception {
        try (var redis = openRedis()) {
            for (var store :
                    List.<ZLinkLocationRepository>of(
                            new ZLinkInMemoryLocationStore(),
                            new ZLinkProviderLocationRepository(redis))) {
                var ownerA = claimOwner(store, "entry-owner-a");
                var ownerB = claimOwner(store, "entry-owner-b");
                var first = descriptor(ownerA, (byte) 1, ENTRY_A);
                var next = descriptor(ownerB, (byte) 2, ENTRY_A);
                var firstKey = key(first);
                assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, first));
                assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, publish(store, next));
                assertEquals(
                        ZLinkLocationWriteStatus.IGNORED_STALE,
                        store.removeMeshNode(firstKey, ownerB).toCompletableFuture().get());
                if (ownerCleanup) {
                    store.removeAllByOwner(ownerA).toCompletableFuture().get();
                    assertEquals(
                            1,
                            store.listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                                    .toCompletableFuture()
                                    .get()
                                    .items()
                                    .size());
                    assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, first));
                    assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, publish(store, next));
                    store.removeAllByOwner(ownerA).toCompletableFuture().get();
                    assertEquals(
                            ZLinkLocationWriteStatus.STORED,
                            store.updateMeshNode(first, ZLinkLocationWriteIntent.RENEW)
                                    .toCompletableFuture()
                                    .get()
                                    .status());
                } else {
                    assertEquals(
                            ZLinkLocationWriteStatus.STORED,
                            store.removeMeshNode(firstKey, ownerA).toCompletableFuture().get());
                }
                assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, next));
                assertEquals(
                        ownerCleanup
                                ? ZLinkLocationWriteStatus.STORED
                                : ZLinkLocationWriteStatus.IGNORED_STALE,
                        store.removeMeshNode(firstKey, ownerA).toCompletableFuture().get());
                assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, publish(store, first));
            }
        }
    }

    @Test
    void entryAndUserSpotReserveShareTheSameNamespace() throws Exception {
        try (var redis = openRedis()) {
            for (var store :
                    List.<ZLinkLocationRepository>of(
                            new ZLinkInMemoryLocationStore(),
                            new ZLinkProviderLocationRepository(redis))) {
                var ownerA = claimOwner(store, "entry-owner-a");
                var ownerB = claimOwner(store, "entry-owner-b");
                var first = descriptor(ownerA, (byte) 1, ENTRY_A);
                var target = descriptor(ownerB, (byte) 2, ENTRY_B);
                assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, first));
                assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, target));
                var kind = ZLinkPlacementObjectKind.USER_SPOT;
                var request =
                        new ZLinkObjectReservationRequest(
                                kind,
                                ZLinkAuthorityKeyCodec.spot(ENTRY_A),
                                "room",
                                "creation-root",
                                new byte[32],
                                32,
                                key(target),
                                target.lifecycleGeneration(),
                                ownerB,
                                new byte[] {1},
                                ZLinkPlacementCapacityBundle.spot(kind, "room", 1));
                assertInstanceOf(
                        ZLinkObjectConflict.class,
                        store.reserve(request, () -> false).toCompletableFuture().get());
                assertEquals(
                        ZLinkLocationWriteStatus.STORED,
                        store.removeMeshNode(key(first), ownerA).toCompletableFuture().get());
                assertInstanceOf(
                        ZLinkObjectReserved.class,
                        store.reserve(request, () -> false).toCompletableFuture().get());
                assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, publish(store, first));
            }
        }
    }

    @Test
    void descriptorRemoveFencesConcurrentClaimRepair() throws Exception {
        try (var redis = openRedis()) {
            var store = new ZLinkProviderLocationRepository(redis);
            var ownerA = claimOwner(store, "entry-owner-a");
            var ownerB = claimOwner(store, "entry-owner-b");
            var first = descriptor(ownerA, (byte) 1, ENTRY_A);
            assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, first));
            var claimKey = new ZLinkStoreKey("entry-spot-id\0" + ENTRY_A);
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    redis.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(), List.of(new ZLinkStoreDelete(claimKey))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());

            var paused = new PausedWriteStore(redis);
            var removal =
                    new ZLinkProviderLocationRepository(paused).removeMeshNode(key(first), ownerA);
            try {
                paused.beforeWrite().get(10, TimeUnit.SECONDS);
                assertEquals(ZLinkLocationWriteStatus.STORED, publish(store, first));
            } finally {
                paused.resumeWrite().complete(null);
            }
            assertEquals(
                    ZLinkLocationWriteStatus.IGNORED_STALE, removal.toCompletableFuture().get());
            assertEquals(
                    1,
                    store.listMeshNodes("mesh", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .get()
                            .items()
                            .size());
            assertEquals(
                    ZLinkLocationWriteStatus.REJECTED_CONFLICT,
                    publish(store, descriptor(ownerB, (byte) 2, ENTRY_A)));
        }
    }

    private record PausedWriteStore(
            ZLinkLocationStore delegate,
            CompletableFuture<Void> beforeWrite,
            CompletableFuture<Void> resumeWrite)
            implements ZLinkLocationStore {
        PausedWriteStore(ZLinkLocationStore delegate) {
            this(delegate, new CompletableFuture<>(), new CompletableFuture<>());
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            beforeWrite.complete(null);
            return resumeWrite.thenCompose(ignored -> delegate.write(request, cancellation));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static ZLinkRedisLocationStore openRedis() {
        return new ZLinkRedisLocationStore(
                new ZLinkRedisLocationOptions()
                        .setConnectionString(ZLinkTestRedis.endpoint())
                        .setKeyPrefix("zlink:entry-descriptor-parity:" + UUID.randomUUID()));
    }

    private static ZLinkLocationWriteStatus publishSecond(
            ZLinkLocationRepository store, String secondEntrySpotId) throws Exception {
        var ownerA = claimOwner(store, "entry-owner-a");
        var ownerB = claimOwner(store, "entry-owner-b");
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                publish(store, descriptor(ownerA, (byte) 1, ENTRY_A)));
        return publish(store, descriptor(ownerB, (byte) 2, secondEntrySpotId));
    }

    private static ZLinkLocationWriteStatus publish(
            ZLinkLocationRepository store, ZLinkMeshNodeDescriptor descriptor) throws Exception {
        return store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get()
                .status();
    }

    private static ZLinkMeshNodeDescriptorKey key(ZLinkMeshNodeDescriptor descriptor) {
        return new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid());
    }

    private static ZLinkLocationOwnerToken claimOwner(ZLinkLocationRepository store, String ownerId)
            throws Exception {
        return assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        store.claimOwnerLease(ownerId, Duration.ofSeconds(30))
                                .toCompletableFuture()
                                .get())
                .token();
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner, byte node, String entrySpotId) {
        var emptyUsage = new ZLinkCapacityUsage(0, 0, 0);
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                RoutingId.from(new byte[] {node}),
                node,
                1,
                "tcp://127.0.0.1:" + (7000 + node),
                Map.of("game", 100),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                "room",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                1)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(entrySpotId),
                100,
                new ZLinkPlacementCapacity(emptyUsage, new ZLinkCapacityUsage(0, 0, 1), List.of()),
                new ZLinkActivationConcurrency(0, 128),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }
}
