package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserveResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectTypeMismatch;
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

final class ZLinkEntrySpotStoreParityTest {
    private static final String ENTRY_SPOT_ID = "play-entry-00000000-0000-4000-8000-000000000001";
    private static final String MESH = "play";
    private static final String TYPE = "room";
    private static final ZLinkPlacementObjectKind USER = ZLinkPlacementObjectKind.USER_SPOT;
    private static final ZLinkPlacementObjectKind INSTANCE = ZLinkPlacementObjectKind.INSTANCE_SPOT;

    @ParameterizedTest
    @EnumSource(
            value = ZLinkPlacementObjectKind.class,
            names = {"USER_SPOT", "INSTANCE_SPOT"})
    void descriptorWritesAndSpotReserveAgreeAcrossStores(ZLinkPlacementObjectKind kind) {
        try (var provider =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix("zlink:entry-spot-parity:" + UUID.randomUUID()))) {
            var redis = writeSequence(new ZLinkProviderLocationRepository(provider), kind);
            var memory = writeSequence(new ZLinkInMemoryLocationStore(), kind);
            assertEquals(
                    new WriteResults(
                            ZLinkLocationWriteStatus.STORED,
                            ZLinkLocationWriteStatus.STORED,
                            ZLinkObjectReserved.class,
                            ZLinkLocationWriteStatus.STORED,
                            ZLinkObjectTypeMismatch.class),
                    redis);
            assertEquals(redis, memory);
        }
    }

    private static WriteResults writeSequence(
            ZLinkLocationRepository store, ZLinkPlacementObjectKind kind) {
        var owner = claim(store, "node-a");
        var first = publish(store, owner);
        var second = publish(store, claim(store, "node-b"));
        var reserved =
                store.reserve(reservation(owner, kind), () -> false).toCompletableFuture().join();
        var third = publish(store, claim(store, "node-c"));
        var competing =
                store.reserve(reservation(owner, kind == USER ? INSTANCE : USER), () -> false)
                        .toCompletableFuture()
                        .join();
        return new WriteResults(first, second, reserved.getClass(), third, competing.getClass());
    }

    private static ZLinkLocationOwnerToken claim(ZLinkLocationRepository store, String node) {
        return assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        store.claimOwnerLease(node, Duration.ofMinutes(1))
                                .toCompletableFuture()
                                .join())
                .token();
    }

    private static ZLinkLocationWriteStatus publish(
            ZLinkLocationRepository store, ZLinkLocationOwnerToken owner) {
        var capabilities =
                List.of(USER, INSTANCE).stream()
                        .map(
                                kind ->
                                        new ZLinkObjectCapability(
                                                kind,
                                                TYPE,
                                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                                false,
                                                8))
                        .toList();
        var descriptor =
                new ZLinkMeshNodeDescriptor(
                        MESH,
                        RoutingId.from(owner.ownerId()),
                        1,
                        1,
                        "tcp://127.0.0.1:7000",
                        Map.of(),
                        1,
                        capabilities,
                        ZLinkMeshNodeObjectRole.SERVER,
                        Optional.of(ENTRY_SPOT_ID),
                        100,
                        new ZLinkPlacementCapacity(
                                new ZLinkCapacityUsage(0, 0, 0),
                                new ZLinkCapacityUsage(0, 0, 8),
                                List.of()),
                        new ZLinkActivationConcurrency(0, 8),
                        Optional.empty(),
                        ZLinkFrameworkRuntimeState.SERVING,
                        owner.ownerId(),
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        Instant.EPOCH);
        return store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join()
                .status();
    }

    private static ZLinkObjectReservationRequest reservation(
            ZLinkLocationOwnerToken owner, ZLinkPlacementObjectKind kind) {
        return new ZLinkObjectReservationRequest(
                kind,
                ZLinkAuthorityKeyCodec.spot(ENTRY_SPOT_ID),
                TYPE,
                "creation-root",
                new byte[32],
                1,
                new ZLinkMeshNodeDescriptorKey(MESH, RoutingId.from(owner.ownerId())),
                1,
                owner,
                new byte[] {1},
                ZLinkPlacementCapacityBundle.spot(kind, TYPE, 1));
    }

    private record WriteResults(
            ZLinkLocationWriteStatus first,
            ZLinkLocationWriteStatus second,
            Class<? extends ZLinkObjectReserveResult> reserve,
            ZLinkLocationWriteStatus third,
            Class<? extends ZLinkObjectReserveResult> competingReserve) {}
}
