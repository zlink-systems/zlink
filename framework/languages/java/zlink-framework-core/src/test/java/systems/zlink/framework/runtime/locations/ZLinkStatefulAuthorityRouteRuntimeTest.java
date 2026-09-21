package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationDeleteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import java.util.function.BooleanSupplier;
import java.util.zip.CRC32C;

final class ZLinkStatefulAuthorityRouteRuntimeTest {
    private static final String MESH_NAME = "game";
    private static final String STABLE_TYPE = "game.room";
    private static final String OWNER_ID = "owner-a";
    private static final long OWNER_LEASE_GENERATION = 31;
    private static final long NODE_GENERATION = 17;
    private static final long OBJECT_GENERATION = 5;
    private static final long AUTHORITY_GENERATION = 7;
    private static final RoutingId SPOT_RID = RoutingId.from("instance-room");
    private static final RoutingId NODE_RID = RoutingId.from("node-a");
    private static final String AUTHORITY_KEY = ZLinkAuthorityKeyCodec.spot(SPOT_RID.toString());

    @Test
    void pendingInstanceAuthorityRegistersIntentBeforeReadyRoutePublication() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var entries =
                new AtomicReference<>(
                        List.of(
                                entry(
                                        "v1",
                                        ZLinkPlacementAllocationState.PENDING,
                                        codec.encodeInstance(
                                                ZLinkServiceAuthorityPayloadCodec.State.CREATING,
                                                STABLE_TYPE,
                                                SPOT_RID.toString(),
                                                OWNER_ID,
                                                OWNER_LEASE_GENERATION,
                                                MESH_NAME,
                                                NODE_RID,
                                                NODE_GENERATION))));
        ZLinkLocationRepository store = authorityStore(entries);
        var recorded = new RecordedNode();
        var failures = new CopyOnWriteArrayList<Throwable>();

        try (var runtime =
                new ZLinkStatefulAuthorityRouteRuntime(
                        store,
                        Map.of(MESH_NAME, recorded.proxy()),
                        Duration.ofHours(1),
                        failures::add)) {
            runtime.start().toCompletableFuture().join();

            assertEquals(1, recorded.registered.size());
            assertTrue(recorded.remembered.isEmpty());
            assertFence(recorded.registered.getFirst());

            entries.set(
                    List.of(
                            entry(
                                    "v2",
                                    ZLinkPlacementAllocationState.ACTIVE,
                                    codec.encodeInstance(
                                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                                            STABLE_TYPE,
                                            SPOT_RID.toString(),
                                            OWNER_ID,
                                            OWNER_LEASE_GENERATION,
                                            MESH_NAME,
                                            NODE_RID,
                                            NODE_GENERATION))));
            runtime.reconcile().toCompletableFuture().join();

            await(
                    () ->
                            recorded.remembered.size() == 1
                                    && recorded.registered.size() == 2
                                    && recorded.forgottenIntents.size() == 1);

            entries.set(List.of());
            runtime.reconcile().toCompletableFuture().join();

            await(
                    () ->
                            recorded.forgottenRoutes.size() == 1
                                    && recorded.forgottenIntents.size() == 2);
            assertTrue(failures.isEmpty(), failures.toString());
        }
    }

    @Test
    void readyRecoveryReplaysBeforeCursorAndPointerCas() throws Exception {
        byte[] envelope = recoveryEnvelope();
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(envelope);
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var entries =
                new AtomicReference<>(
                        List.of(
                                entry(
                                        "v1",
                                        ZLinkPlacementAllocationState.ACTIVE,
                                        codec.encodeInstance(
                                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                                STABLE_TYPE,
                                                SPOT_RID.toString(),
                                                OWNER_ID,
                                                OWNER_LEASE_GENERATION,
                                                MESH_NAME,
                                                NODE_RID,
                                                NODE_GENERATION,
                                                Optional.of(
                                                        new ZLinkServiceAuthorityPayloadCodec
                                                                .ActivationRecoveryState(
                                                                "activation-root",
                                                                digest,
                                                                envelope.length,
                                                                1,
                                                                0))))));
        var versions = new AtomicInteger(1);
        var events = new CopyOnWriteArrayList<String>();
        ZLinkLocationRepository store = authorityStore(entries, versions, events);
        ZLinkRelocationStore relocation = relocationStore(envelope, events);
        var recorded = new RecordedNode(events);

        try (var runtime =
                new ZLinkStatefulAuthorityRouteRuntime(
                        store,
                        relocation,
                        Map.of(MESH_NAME, recorded.proxy()),
                        Duration.ofHours(1),
                        failure -> {
                            throw new AssertionError(failure);
                        })) {
            runtime.start().toCompletableFuture().join();
        }

        assertEquals(List.of("replay", "cursor", "release", "delete"), events);
        var recovered = codec.decode(entries.get().getFirst().snapshot().payload()).orElseThrow();
        assertTrue(recovered.activationRecoveryState().isEmpty());
        assertEquals(1, recorded.registered.size());
    }

    private static ZLinkAuthorityEntry entry(
            String storeVersion, ZLinkPlacementAllocationState state, byte[] payload) {
        return new ZLinkAuthorityEntry(
                AUTHORITY_KEY,
                new ZLinkAuthoritySnapshot(
                        storeVersion,
                        payload,
                        OBJECT_GENERATION,
                        AUTHORITY_GENERATION,
                        OWNER_ID,
                        OWNER_LEASE_GENERATION,
                        new ZLinkPlacementAllocation(
                                state,
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                STABLE_TYPE,
                                new ZLinkMeshNodeDescriptorKey(MESH_NAME, NODE_RID),
                                NODE_GENERATION,
                                ZLinkPlacementCapacityBundle.spot(
                                        ZLinkPlacementObjectKind.INSTANCE_SPOT, STABLE_TYPE, 1)),
                        Instant.EPOCH));
    }

    private static ZLinkLocationRepository authorityStore(
            AtomicReference<List<ZLinkAuthorityEntry>> entries) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("list")) {
                                return CompletableFuture.completedFuture(
                                        new ZLinkAuthorityPage(entries.get(), Optional.empty()));
                            }
                            if (method.getDeclaringClass() == Object.class) {
                                return method.invoke(entries, arguments);
                            }
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static ZLinkLocationRepository authorityStore(
            AtomicReference<List<ZLinkAuthorityEntry>> entries,
            AtomicInteger versions,
            List<String> events) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("list")) {
                                return CompletableFuture.completedFuture(
                                        new ZLinkAuthorityPage(entries.get(), Optional.empty()));
                            }
                            if (method.getName().equals("compareExchange")) {
                                ZLinkAuthorityEntry current = entries.get().getFirst();
                                ZLinkAuthorityExpectFound expected =
                                        (ZLinkAuthorityExpectFound) arguments[1];
                                assertEquals(
                                        current.snapshot().storeVersion(), expected.storeVersion());
                                ZLinkAuthorityPut put = (ZLinkAuthorityPut) arguments[2];
                                String version = "v" + versions.incrementAndGet();
                                var snapshot = current.snapshot();
                                var stored =
                                        new ZLinkAuthorityStored(
                                                version,
                                                put.payload(),
                                                snapshot.objectGeneration(),
                                                snapshot.authorityOwnerGeneration(),
                                                snapshot.ownerId(),
                                                snapshot.ownerLeaseGeneration(),
                                                snapshot.allocation(),
                                                Instant.EPOCH);
                                entries.set(
                                        List.of(
                                                new ZLinkAuthorityEntry(
                                                        current.key(),
                                                        new ZLinkAuthoritySnapshot(
                                                                stored.storeVersion(),
                                                                stored.payload(),
                                                                stored.objectGeneration(),
                                                                stored.authorityOwnerGeneration(),
                                                                stored.ownerId(),
                                                                stored.ownerLeaseGeneration(),
                                                                stored.allocation(),
                                                                stored.storeNow()))));
                                var decoded =
                                        new ZLinkServiceAuthorityPayloadCodec()
                                                .decode(stored.payload())
                                                .orElseThrow();
                                events.add(
                                        decoded.activationRecoveryState().isPresent()
                                                ? "cursor"
                                                : "release");
                                return CompletableFuture.completedFuture(stored);
                            }
                            if (method.getDeclaringClass() == Object.class) {
                                return method.invoke(entries, arguments);
                            }
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static ZLinkRelocationStore relocationStore(byte[] envelope, List<String> events) {
        return (ZLinkRelocationStore)
                Proxy.newProxyInstance(
                        ZLinkRelocationStore.class.getClassLoader(),
                        new Class<?>[] {ZLinkRelocationStore.class},
                        (proxy, method, arguments) ->
                                switch (method.getName()) {
                                    case "get" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkRelocationFound(envelope));
                                    case "delete" -> {
                                        events.add("delete");
                                        yield CompletableFuture.completedFuture(
                                                ZLinkRelocationDeleteResult.DELETED);
                                    }
                                    case "toString" -> "RecoveryStore";
                                    case "hashCode" -> System.identityHashCode(proxy);
                                    case "equals" -> proxy == arguments[0];
                                    default ->
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                });
    }

    private static byte[] recoveryEnvelope() {
        var payloadCodec = new ZLinkServiceM6AWireCodec();
        byte[] application =
                payloadCodec.encodeApplicationPayload(
                        new ZLinkServiceM6AWireCodec.ApplicationPayload(
                                "packet",
                                "application/json",
                                "{}".getBytes(StandardCharsets.UTF_8)));
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        text8(body, SPOT_RID.toString());
        text8(body, STABLE_TYPE);
        text8(body, MESH_NAME);
        text8(body, NODE_RID.toString());
        u64(body, NODE_GENERATION);
        text8(body, "41");
        text8(body, "source-node");
        u64(body, 19);
        body.write(0);
        body.write(1);
        u64(body, 1);
        u64(body, 2);
        u64(body, 1000);
        body.write(0);
        body.writeBytes(application);
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        result.writeBytes(new byte[] {0x5a, 0x4c, 0x49, 0x41, 1, 0, 0});
        u32(result, body.size());
        result.writeBytes(body.toByteArray());
        CRC32C crc = new CRC32C();
        byte[] withoutChecksum = result.toByteArray();
        crc.update(withoutChecksum, 0, withoutChecksum.length);
        u32(result, crc.getValue());
        return result.toByteArray();
    }

    private static void text8(ByteArrayOutputStream output, String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        output.write(bytes.length);
        output.writeBytes(bytes);
    }

    private static void u32(ByteArrayOutputStream output, long value) {
        output.writeBytes(
                ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt((int) value).array());
    }

    private static void u64(ByteArrayOutputStream output, long value) {
        output.writeBytes(
                ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(value).array());
    }

    private static void assertFence(ZLinkServiceM6BWireCodec.InstanceRouteFence fence) {
        assertEquals(NODE_RID, fence.targetNodeRid());
        assertEquals(NODE_GENERATION, fence.targetNodeGeneration());
        assertEquals(SPOT_RID.toString(), fence.targetSpotId());
        assertEquals(OBJECT_GENERATION, fence.objectGeneration());
        assertEquals(AUTHORITY_GENERATION, fence.authorityOwnerGeneration());
        assertEquals(OWNER_LEASE_GENERATION, fence.leaseGeneration());
    }

    private static void await(BooleanSupplier condition) {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            LockSupport.parkNanos(Duration.ofMillis(5).toNanos());
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class RecordedNode {
        private final List<String> events;
        private final List<ZLinkInternalMeshNode.SpotAuthorityRoute> remembered =
                new CopyOnWriteArrayList<>();
        private final List<ZLinkInternalMeshNode.SpotAuthorityRoute> forgottenRoutes =
                new CopyOnWriteArrayList<>();
        private final List<ZLinkServiceM6BWireCodec.InstanceRouteFence> registered =
                new CopyOnWriteArrayList<>();
        private final List<ZLinkServiceM6BWireCodec.InstanceRouteFence> forgottenIntents =
                new CopyOnWriteArrayList<>();

        RecordedNode() {
            this(new CopyOnWriteArrayList<>());
        }

        RecordedNode(List<String> events) {
            this.events = events;
        }

        ZLinkInternalMeshNode proxy() {
            return (ZLinkInternalMeshNode)
                    Proxy.newProxyInstance(
                            ZLinkInternalMeshNode.class.getClassLoader(),
                            new Class<?>[] {ZLinkInternalMeshNode.class},
                            (proxy, method, arguments) -> {
                                switch (method.getName()) {
                                    case "rememberSpotAuthority" ->
                                            remembered.add(
                                                    (ZLinkInternalMeshNode.SpotAuthorityRoute)
                                                            arguments[0]);
                                    case "forgetSpotAuthority" ->
                                            forgottenRoutes.add(
                                                    (ZLinkInternalMeshNode.SpotAuthorityRoute)
                                                            arguments[0]);
                                    case "registerInstanceIntent" ->
                                            registered.add(
                                                    (ZLinkServiceM6BWireCodec.InstanceRouteFence)
                                                            arguments[1]);
                                    case "forgetInstanceIntent" ->
                                            forgottenIntents.add(
                                                    (ZLinkServiceM6BWireCodec.InstanceRouteFence)
                                                            arguments[0]);
                                    case "recoverInstanceActivation" -> {
                                        events.add("replay");
                                        return CompletableFuture.completedFuture(null);
                                    }
                                    case "routingId" -> {
                                        return NODE_RID;
                                    }
                                    case "lifecycleGeneration" -> {
                                        return NODE_GENERATION;
                                    }
                                    case "status" -> {
                                        return new MeshNodeStatus(
                                                MeshNodeState.READY,
                                                NODE_RID,
                                                MESH_NAME,
                                                "",
                                                NODE_GENERATION,
                                                41,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0);
                                    }
                                    case "name" -> {
                                        return "recorded-node";
                                    }
                                    case "toString" -> {
                                        return "RecordedNode";
                                    }
                                    case "hashCode" -> {
                                        return System.identityHashCode(proxy);
                                    }
                                    case "equals" -> {
                                        return proxy == arguments[0];
                                    }
                                    default -> {}
                                }
                                return null;
                            });
        }
    }
}
