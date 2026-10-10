package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.*;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;

final class StoredInstanceActivationTest {
    @ParameterizedTest
    @CsvSource({
        "mesh, false",
        "type, false",
        "descriptor, false",
        "deadline, false",
        "operation, false",
        "metadata, false",
        "metadataBytes, false",
        "match, false",
        "mesh, true",
        "type, true",
        "descriptor, true",
        "deadline, true",
        "operation, true",
        "metadata, true",
        "metadataBytes, true",
        "match, true"
    })
    void storedActivationMismatchPreservesStore(String field, boolean ready) throws Exception {
        var recovery = new InMemoryRelocationStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(recovery);
        options.addRouteMesh("mesh")
                .listen("inproc://stored-" + UUID.randomUUID())
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "room",
                        InstanceSpotColdActivationTest.Instance.class,
                        factory -> factory.disableRelocation());
        try (var part = systems.zlink.contracts.messaging.Message.from(new byte[] {1});
                var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var host = (ZLinkSpotRuntime) runtime.spotManager();
            var repositoryField = ZLinkSpotRuntime.class.getDeclaredField("userSpotLocationStore");
            repositoryField.setAccessible(true);
            var store = (ZLinkLocationRepository) repositoryField.get(host);
            var descriptor =
                    store.listMeshNodes("mesh", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .join()
                            .items()
                            .get(0);
            var envelope =
                    new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                            "spot",
                            "room",
                            "mesh",
                            descriptor.rid(),
                            descriptor.lifecycleGeneration(),
                            "1",
                            RoutingId.from("source"),
                            5,
                            Optional.empty(),
                            false,
                            1,
                            2,
                            null,
                            System.currentTimeMillis() + 5000,
                            field.equals("metadataBytes")
                                    ? new byte[] {1, 1, 1, 107, 0, 1, 118}
                                    : new byte[0],
                            ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(List.of(part)));
            var stored =
                    new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                            envelope.targetSpotId(),
                            field.equals("type") ? "other" : envelope.stableType(),
                            field.equals("mesh") ? "other" : envelope.targetMeshName(),
                            envelope.targetNodeRid(),
                            envelope.targetNodeGeneration(),
                            field.equals("descriptor") ? "2" : envelope.descriptorVersion(),
                            envelope.sourceNodeRid(),
                            envelope.sourceNodeGeneration(),
                            envelope.sourceSpotId(),
                            envelope.request(),
                            envelope.operationHigh(),
                            field.equals("operation") ? 3 : envelope.operationLow(),
                            envelope.replyRouteId(),
                            envelope.deadlineUnixMs()
                                    + (field.equals("deadline") || field.equals("operation")
                                            ? 1
                                            : 0),
                            field.startsWith("metadata")
                                    ? new byte[] {1, 0}
                                    : envelope.metadataFrame(),
                            envelope.applicationPayloadFrame());
            byte[] root = new ZLinkInstanceActivationRecoveryCodec().encode(stored);
            var receipt =
                    recovery.put(root, Duration.ofMinutes(1), () -> false)
                            .toCompletableFuture()
                            .join();
            String key = ZLinkAuthorityKeyCodec.spot("spot");
            var payload =
                    new ZLinkServiceAuthorityPayloadCodec()
                            .encodeInstance(
                                    ZLinkServiceAuthorityPayloadCodec.State.CREATING,
                                    "room",
                                    "spot",
                                    descriptor.ownerId(),
                                    descriptor.leaseGeneration(),
                                    "mesh",
                                    descriptor.rid(),
                                    descriptor.lifecycleGeneration());
            var reserved =
                    store.reserve(
                                    new ZLinkObjectReservationRequest(
                                            ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                            key,
                                            "room",
                                            receipt.reference(),
                                            java.security.MessageDigest.getInstance("SHA-256")
                                                    .digest(root),
                                            root.length,
                                            new ZLinkMeshNodeDescriptorKey(
                                                    "mesh", descriptor.rid()),
                                            descriptor.lifecycleGeneration(),
                                            new ZLinkLocationOwnerToken(
                                                    descriptor.ownerId(),
                                                    descriptor.leaseGeneration()),
                                            payload,
                                            ZLinkPlacementCapacityBundle.spot(
                                                    ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                                    "room",
                                                    1)),
                                    () -> false)
                            .toCompletableFuture()
                            .join();
            var reservation = assertInstanceOf(ZLinkObjectReserved.class, reserved).reservation();
            if (ready) {
                byte[] readyPayload =
                        new ZLinkServiceAuthorityPayloadCodec()
                                .encodeInstance(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        "room",
                                        "spot",
                                        descriptor.ownerId(),
                                        descriptor.leaseGeneration(),
                                        "mesh",
                                        descriptor.rid(),
                                        descriptor.lifecycleGeneration(),
                                        Optional.of(
                                                new ZLinkServiceAuthorityPayloadCodec
                                                        .ActivationRecoveryState(
                                                        receipt.reference(),
                                                        java.security.MessageDigest.getInstance(
                                                                        "SHA-256")
                                                                .digest(root),
                                                        root.length,
                                                        1,
                                                        0)));
                assertEquals(
                        ZLinkObjectCommitResult.COMMITTED,
                        store.commit(reservation, readyPayload, () -> false)
                                .toCompletableFuture()
                                .join());
            }
            var capacityBefore =
                    store.listMeshNodes("mesh", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .join()
                            .items()
                            .get(0)
                            .capacity();
            var before =
                    (ZLinkAuthoritySnapshot)
                            store.read(key, () -> false).toCompletableFuture().join();
            var method =
                    ZLinkSpotRuntime.class.getDeclaredMethod(
                            (field.equals("match") || field.equals("operation"))
                                    ? "readInstanceSpotActivationTarget"
                                    : "reserveInstanceSpotTarget",
                            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope.class);
            method.setAccessible(true);
            var result = (CompletionStage<?>) method.invoke(host, envelope);
            if ((field.equals("match") || field.equals("operation"))) {
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        result.toCompletableFuture().get(5, TimeUnit.SECONDS));
            } else {
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () -> result.toCompletableFuture().get(5, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            }
            var after =
                    (ZLinkAuthoritySnapshot)
                            store.read(key, () -> false).toCompletableFuture().join();
            assertEquals(before.storeVersion(), after.storeVersion());
            assertEquals(
                    capacityBefore,
                    store.listMeshNodes("mesh", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .join()
                            .items()
                            .get(0)
                            .capacity());
            assertArrayEquals(before.payload(), after.payload());
            assertArrayEquals(
                    root,
                    ((ZLinkRelocationFound)
                                    recovery.get(receipt.reference(), () -> false)
                                            .toCompletableFuture()
                                            .join())
                            .payload());
        }
    }
}
