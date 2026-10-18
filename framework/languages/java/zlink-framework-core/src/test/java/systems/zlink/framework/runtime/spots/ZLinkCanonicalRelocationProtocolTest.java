package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.UUID;

final class ZLinkCanonicalRelocationProtocolTest {
    private static final UUID RELOCATION = UUID.fromString("00000000-0000-0001-0000-000000000002");
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");

    @Test
    void prepareDeadlineMatchesSharedGoldenAndRejectsMissingOrZeroTail() throws Exception {
        try (var input = getClass().getResourceAsStream("/relocation-control-v1.json")) {
            var fixture = new ObjectMapper().readTree(input);
            for (var entry : fixture.path("canonical")) {
                if (entry.path("command").asInt()
                        != ServiceWireConstants.COMMAND_RELOCATION_PREPARE) continue;
                byte[] bytes = java.util.HexFormat.of().parseHex(entry.path("hex").asText());
                var decoded = ZLinkCanonicalRelocationProtocol.decodePrepare(bytes);
                assertEquals(
                        Long.parseUnsignedLong(
                                entry.path("decoded").path("deadlineUnixMs").asText()),
                        decoded.deadlineUnixMs());
                assertArrayEquals(bytes, ZLinkCanonicalRelocationProtocol.encodePrepare(decoded));
                byte[] truncated = java.util.Arrays.copyOf(bytes, bytes.length - 1);
                assertThrows(
                        IllegalArgumentException.class,
                        () -> ZLinkCanonicalRelocationProtocol.decodePrepare(truncated));
                byte[] zero = bytes.clone();
                java.util.Arrays.fill(zero, zero.length - Long.BYTES, zero.length, (byte) 0);
                assertThrows(
                        IllegalArgumentException.class,
                        () -> ZLinkCanonicalRelocationProtocol.decodePrepare(zero));
                return;
            }
            throw new AssertionError("shared command 40 vector is missing");
        }
    }

    @Test
    void directTransferControlsRoundTripWithExplicitTargetFailure() {
        var coordinator =
                new ZLinkCanonicalRelocationProtocol.Coordinator(
                        "source-owner", 3, SOURCE, 4, "store-v5");
        var target = new ZLinkCanonicalRelocationProtocol.Target(TARGET, 6, "target-owner", 7);
        var object = new ZLinkCanonicalRelocationProtocol.ObjectFence(1, "actor", "", 8, 9);
        var prepare =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                        RELOCATION,
                        10,
                        coordinator,
                        target,
                        ZLinkCanonicalRelocationProtocol.SOURCE,
                        object,
                        SOURCE,
                        4,
                        new ZLinkCanonicalRelocationProtocol.Manifest(3, 1, 0),
                        12,
                        13);
        var ready =
                new ZLinkCanonicalRelocationProtocol.Ready(
                        RELOCATION,
                        10,
                        coordinator,
                        target,
                        object,
                        ZLinkCanonicalRelocationProtocol.TARGET);
        var failed =
                new ZLinkCanonicalRelocationProtocol.Failed(
                        RELOCATION,
                        10,
                        coordinator,
                        target,
                        object,
                        ZLinkCanonicalRelocationProtocol.TARGET,
                        ServiceWireConstants.FRAMEWORK_ERROR_RELOCATION_DATA_LOST);
        var data =
                new ZLinkCanonicalRelocationProtocol.Data(
                        RELOCATION,
                        10,
                        coordinator,
                        ZLinkCanonicalRelocationProtocol.SOURCE,
                        object,
                        new byte[] {13, 14, 15});
        var cutover =
                new ZLinkCanonicalRelocationProtocol.Cutover(
                        RELOCATION,
                        10,
                        coordinator,
                        ZLinkCanonicalRelocationProtocol.SOURCE,
                        object,
                        0,
                        0);

        assertEquals(
                prepare,
                ZLinkCanonicalRelocationProtocol.decodePrepare(
                        ZLinkCanonicalRelocationProtocol.encodePrepare(prepare)));
        assertEquals(
                ready,
                ZLinkCanonicalRelocationProtocol.decodeReady(
                        ZLinkCanonicalRelocationProtocol.encodeReady(ready)));
        assertEquals(
                failed,
                ZLinkCanonicalRelocationProtocol.decodeFailed(
                        ZLinkCanonicalRelocationProtocol.encodeFailed(failed)));
        var decodedData =
                ZLinkCanonicalRelocationProtocol.decodeData(
                        ZLinkCanonicalRelocationProtocol.encodeData(data));
        assertEquals(data.id(), decodedData.id());
        assertEquals(data.targetAttemptGeneration(), decodedData.targetAttemptGeneration());
        assertEquals(data.coordinator(), decodedData.coordinator());
        assertEquals(data.object(), decodedData.object());
        assertArrayEquals(data.frozenRecord(), decodedData.frozenRecord());
        assertEquals(
                cutover,
                ZLinkCanonicalRelocationProtocol.decodeCutover(
                        ZLinkCanonicalRelocationProtocol.encodeCutover(cutover)));

        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                Byte.toUnsignedInt(ZLinkCanonicalRelocationProtocol.encodePrepare(prepare)[3]));
        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_READY,
                Byte.toUnsignedInt(ZLinkCanonicalRelocationProtocol.encodeReady(ready)[3]));
        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_FAILED,
                Byte.toUnsignedInt(ZLinkCanonicalRelocationProtocol.encodeFailed(failed)[3]));
        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_DATA,
                Byte.toUnsignedInt(ZLinkCanonicalRelocationProtocol.encodeData(data)[3]));
        assertEquals(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                Byte.toUnsignedInt(ZLinkCanonicalRelocationProtocol.encodeCutover(cutover)[3]));
    }
}
