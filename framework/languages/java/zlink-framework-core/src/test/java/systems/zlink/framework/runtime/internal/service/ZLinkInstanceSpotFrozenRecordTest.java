package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

import java.io.IOException;
import java.util.List;
import java.util.Optional;

final class ZLinkInstanceSpotFrozenRecordTest {
    private static final long SOURCE_GENERATION = 7;
    private static final long REQUEST_ID_HIGH = Long.MIN_VALUE;
    private static final long REQUEST_ID_LOW = 37;
    private static final long REPLY_ROUTE_ID = 41;
    private static final byte[] METADATA = {1, 1, 1, 'k', 0, 1, 'v'};
    private static final byte[] PAYLOAD = {3, 5, 8};

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void acceptedInstanceOperationUsesTheSchemaFormat(boolean request) throws IOException {
        byte[] bytes = encodeAccepted(request);
        var wire =
                ServiceWireCodec.decodeFrozenRecord(
                        bytes, ZLinkServiceM6BWireCodec.generatedContext());
        assertEquals(ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION, wire.recordKind());
        assertEquals(
                request
                        ? ServiceWireCodec.MeshOperationKind.INSTANCE_SPOT_REQUEST
                        : ServiceWireCodec.MeshOperationKind.NONE,
                wire.operationKind());
        assertEquals(request ? REQUEST_ID_HIGH : 0, wire.operationId().high().value());
        assertEquals(request ? REQUEST_ID_LOW : 0, wire.operationId().low().value());
        var body =
                assertInstanceOf(
                        ServiceWireCodec.FrozenRecordBodyInstanceSpotActivation.class, wire.body());
        assertEquals(SOURCE_GENERATION, body.sourceNodeGeneration().value());
        assertEquals(
                request
                        ? ServiceWireCodec.InstanceOperationKind.REQUEST
                        : ServiceWireCodec.InstanceOperationKind.SEND,
                body.operationKind());
        var route = assertInstanceOf(ServiceWireCodec.InstanceRouteV1Ready.class, body.route());
        assertEquals(RoutingId.from("target"), RoutingId.from(route.targetNodeRid().value()));
        assertEquals("room", route.targetSpotId().value());
        assertEquals(17, route.authority().objectGeneration().value());
        assertEquals("target-owner", route.authority().ownerId().value());
        assertEquals("store-version", route.authority().storeVersion().value());
        assertEquals(ServiceWireCodec.Bool8.TRUE, route.instanceIntent());

        var envelope =
                new ServiceWirePilotCodec.RelocationEnvelopeV1(
                        1,
                        2,
                        new ServiceWirePilotCodec.RelocationInstanceSpotIdentity(
                                "Room", "room", 17),
                        0,
                        List.of(
                                new ServiceWirePilotCodec.RelocationApplicationState(
                                        1, false, new byte[0])),
                        List.of(new ServiceWirePilotCodec.RelocationSavedWork(1, 1, bytes)),
                        List.of(),
                        List.of(),
                        List.of());
        var restored =
                ZLinkServiceRelocationEnvelopeCodec.decode(
                        ZLinkServiceRelocationEnvelopeCodec.encode(envelope));
        assertArrayEquals(bytes, restored.savedWork().getFirst().frozenRecord());
        var decoded =
                ZLinkServiceFrozenRecordCodec.decodeSpot(
                        restored.savedWork().getFirst().frozenRecord());
        assertEquals("room", decoded.targetSpotId());
        assertEquals(17, decoded.objectGeneration());
        assertEquals(SOURCE_GENERATION, decoded.sourceNodeGeneration());
        assertEquals(Optional.of("source-room"), decoded.sourceSpotId());
        assertEquals(
                request ? Optional.of(REPLY_ROUTE_ID) : Optional.empty(), decoded.replyRouteId());
        assertArrayEquals(METADATA, decoded.metadataFrame());
        assertArrayEquals(PAYLOAD, decoded.payload());

        var context = ZLinkServiceM6BWireCodec.generatedContext();
        byte[] relay =
                ServiceWireCodec.encodeCommand(
                        new ServiceWireCodec.RelocationDataCommand(
                                0,
                                new ServiceWireCodec.RelocationId(
                                        new ServiceWireCodec.U64(1), new ServiceWireCodec.U64(2)),
                                new ServiceWireCodec.NonzeroU64(1),
                                new ServiceWireCodec.RelocationCoordinatorFence(
                                        new ServiceWireCodec.Text8("source-owner"),
                                        new ServiceWireCodec.NonzeroU64(11),
                                        new ServiceWireCodec.Rid(
                                                RoutingId.from("source").toBytes()),
                                        new ServiceWireCodec.NonzeroU64(SOURCE_GENERATION),
                                        new ServiceWireCodec.AuthorityStoreVersion(
                                                "store-version")),
                                ServiceWireCodec.RelocationRole.SOURCE,
                                new ServiceWireCodec.RelocationObjectIdentityInstanceSpot(
                                        ServiceWireCodec.StatefulObjectKind.INSTANCE_SPOT,
                                        new ServiceWireCodec.Text8("Room"),
                                        new ServiceWireCodec.Text8("room"),
                                        new ServiceWireCodec.NonzeroU64(17)),
                                wire),
                        context);
        assertArrayEquals(
                relay, new ZLinkCanonicalRelocationControlCodec().decode(relay).encoded());
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void readerRejectsAnOperationIdThatDisagreesWithTheInstanceOperation(boolean request)
            throws IOException {
        var wire =
                ServiceWireCodec.decodeFrozenRecord(
                        encodeAccepted(request), ZLinkServiceM6BWireCodec.generatedContext());
        var invalid =
                new ServiceWireCodec.FrozenRecord(
                        wire.recordKind(),
                        wire.source(),
                        wire.hasMetadata(),
                        wire.metadata(),
                        new ServiceWireCodec.OperationIdOrZero(
                                new ServiceWireCodec.U64(0),
                                new ServiceWireCodec.U64(request ? 0 : 1)),
                        wire.operationKind(),
                        wire.replyRoute(),
                        wire.body());
        byte[] bytes =
                ServiceWireCodec.encodeFrozenRecord(
                        invalid, ZLinkServiceM6BWireCodec.generatedContext());
        assertThrows(
                IllegalArgumentException.class,
                () -> ZLinkServiceFrozenRecordCodec.decodeSpot(bytes));
    }

    private static byte[] encodeAccepted(boolean request) {
        RoutingId sourceRid = RoutingId.from("source");
        var source =
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                        sourceRid, SOURCE_GENERATION, "source-owner", 11);
        var target =
                new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                        RoutingId.from("target"),
                        13,
                        "room",
                        17,
                        "target-owner",
                        19,
                        23,
                        "store-version");
        var operation =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        0,
                        target,
                        true,
                        SOURCE_GENERATION,
                        sourceRid,
                        "source-room",
                        request,
                        request ? REQUEST_ID_HIGH : 0,
                        request ? REQUEST_ID_LOW : 0,
                        request ? REPLY_ROUTE_ID : null);
        byte[] payload =
                new ZLinkServiceM6AWireCodec()
                        .encodeApplicationPayload(
                                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                                        "room-packet", "application/octet-stream", PAYLOAD));
        return ZLinkServiceFrozenRecordCodec.encodeInstanceSpot(
                source, operation, target, METADATA, payload);
    }
}
