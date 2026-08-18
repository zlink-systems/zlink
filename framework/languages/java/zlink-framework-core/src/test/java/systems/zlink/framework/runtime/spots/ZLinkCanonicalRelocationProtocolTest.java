package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationProtocolTest {
    private static final UUID RELOCATION =
        UUID.fromString("00000000-0000-0001-0000-000000000002");
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");

    @Test
    void directTransferControlsRoundTripWithExplicitTargetFailure() {
        var coordinator = new ZLinkCanonicalRelocationProtocol.Coordinator(
            "source-owner", 3, SOURCE, 4, "store-v5");
        var target = new ZLinkCanonicalRelocationProtocol.Target(
            TARGET, 6, "target-owner", 7);
        var object = new ZLinkCanonicalRelocationProtocol.ObjectFence(
            1, "actor", "", 8, 9);
        var prepare = new ZLinkCanonicalRelocationProtocol.Prepare(
            RELOCATION, 10, coordinator, target,
            ZLinkCanonicalRelocationProtocol.SOURCE, object,
            SOURCE, 4,
            new ZLinkCanonicalRelocationProtocol.Manifest(3, 1, 0), 12);
        var ready = new ZLinkCanonicalRelocationProtocol.Ready(
            RELOCATION, 10, coordinator, target, object,
            ZLinkCanonicalRelocationProtocol.TARGET);
        var failed = new ZLinkCanonicalRelocationProtocol.Failed(
            RELOCATION, 10, coordinator, target, object,
            ZLinkCanonicalRelocationProtocol.TARGET,
            ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST);
        var data = new ZLinkCanonicalRelocationProtocol.Data(
            RELOCATION, 10, coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE, object,
            new byte[] {13, 14, 15});
        var cutover = new ZLinkCanonicalRelocationProtocol.Cutover(
            RELOCATION, 10, coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE, object, 0, 0);

        assertEquals(prepare, ZLinkCanonicalRelocationProtocol.decodePrepare(
            ZLinkCanonicalRelocationProtocol.encodePrepare(prepare)));
        assertEquals(ready, ZLinkCanonicalRelocationProtocol.decodeReady(
            ZLinkCanonicalRelocationProtocol.encodeReady(ready)));
        assertEquals(failed, ZLinkCanonicalRelocationProtocol.decodeFailed(
            ZLinkCanonicalRelocationProtocol.encodeFailed(failed)));
        var decodedData = ZLinkCanonicalRelocationProtocol.decodeData(
            ZLinkCanonicalRelocationProtocol.encodeData(data));
        assertEquals(data.id(), decodedData.id());
        assertEquals(data.targetAttemptGeneration(),
            decodedData.targetAttemptGeneration());
        assertEquals(data.coordinator(), decodedData.coordinator());
        assertEquals(data.object(), decodedData.object());
        assertArrayEquals(data.frozenRecord(), decodedData.frozenRecord());
        assertEquals(cutover,
            ZLinkCanonicalRelocationProtocol.decodeCutover(
                ZLinkCanonicalRelocationProtocol.encodeCutover(cutover)));

        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
            Byte.toUnsignedInt(
                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare)[3]));
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_READY,
            Byte.toUnsignedInt(
                ZLinkCanonicalRelocationProtocol.encodeReady(ready)[3]));
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_FAILED,
            Byte.toUnsignedInt(
                ZLinkCanonicalRelocationProtocol.encodeFailed(failed)[3]));
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_DATA,
            Byte.toUnsignedInt(
                ZLinkCanonicalRelocationProtocol.encodeData(data)[3]));
        assertEquals(ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
            Byte.toUnsignedInt(
                ZLinkCanonicalRelocationProtocol.encodeCutover(cutover)[3]));
    }

    @Test
    void baseChecksumAndPayloadStageRoundTrip() {
        var coordinator = new ZLinkCanonicalRelocationProtocol.Coordinator(
            "source-owner", 3, SOURCE, 4, "store-v5");
        var target = new ZLinkCanonicalRelocationProtocol.Target(
            TARGET, 6, "target-owner", 7);
        var object = new ZLinkCanonicalRelocationProtocol.ObjectFence(
            1, "actor", "", 8, 9);
        var manifestWithBase = new ZLinkCanonicalRelocationProtocol.Manifest(
            3, 1, 700221333L, 1849572196L);
        assertEquals(1849572196L, manifestWithBase.baseChecksumCrc32c());
        var prepare = new ZLinkCanonicalRelocationProtocol.Prepare(
            RELOCATION, 10, coordinator, target,
            ZLinkCanonicalRelocationProtocol.SOURCE, object,
            SOURCE, 4, manifestWithBase, 12);
        assertEquals(prepare, ZLinkCanonicalRelocationProtocol.decodePrepare(
            ZLinkCanonicalRelocationProtocol.encodePrepare(prepare)));

        var baseChunk = new ZLinkCanonicalRelocationProtocol.State(
            RELOCATION, 10, coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE, object,
            ZLinkCanonicalRelocationProtocol.PAYLOAD_STAGE_BASE, 0,
            new byte[] {1, 2, 3});
        var decodedBase = ZLinkCanonicalRelocationProtocol.decodeState(
            ZLinkCanonicalRelocationProtocol.encodeState(baseChunk));
        assertEquals(baseChunk.payloadStage(), decodedBase.payloadStage());
        assertEquals(baseChunk.chunkOrdinal(), decodedBase.chunkOrdinal());
        assertArrayEquals(baseChunk.chunkData(), decodedBase.chunkData());

        var finalChunk = new ZLinkCanonicalRelocationProtocol.State(
            RELOCATION, 10, coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE, object, 0,
            new byte[] {4, 5, 6});
        assertEquals(
            ZLinkCanonicalRelocationProtocol.PAYLOAD_STAGE_FINAL,
            finalChunk.payloadStage());
        var decodedFinal = ZLinkCanonicalRelocationProtocol.decodeState(
            ZLinkCanonicalRelocationProtocol.encodeState(finalChunk));
        assertEquals(finalChunk.payloadStage(), decodedFinal.payloadStage());
        assertEquals(finalChunk.chunkOrdinal(), decodedFinal.chunkOrdinal());
        assertArrayEquals(finalChunk.chunkData(), decodedFinal.chunkData());
    }
}
