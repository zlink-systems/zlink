package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkServiceWireCodecTest {
    private final ZLinkServiceWireCodec codec = new ZLinkServiceWireCodec();

    @Test
    void livenessProbeRoundTripsAsOneThirteenByteHeadFrame() {
        List<byte[]> encoded = codec.encode(new ZLinkServiceWireFrame(
            ServiceWireConstants.COMMAND_LIVENESS_PROBE,
            0,
            List.of(new byte[] {0, 0, 0, 0, 0, 0, 0, 7})));

        assertEquals(1, encoded.size());
        assertArrayEquals(
            new byte[] {0x5a, 0x4d, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0, 7},
            encoded.getFirst());
        ZLinkServiceWireFrame decoded = codec.decode(encoded);
        assertEquals(ServiceWireConstants.COMMAND_LIVENESS_PROBE, decoded.command());
        assertArrayEquals(new byte[] {0, 0, 0, 0, 0, 0, 0, 7}, decoded.frames().getFirst());
    }

    @Test
    void rejectsUnknownFlagBeforeMailboxAdmission() {
        assertThrows(ZLinkServiceWireException.class, () -> codec.decode(List.of(
            new byte[] {0x5a, 0x4d, 1, 1, 0x10})));
    }

    @Test
    void rejectsZeroAndTruncatedLivenessProbeIds() {
        assertThrows(ZLinkServiceWireException.class, () -> codec.decode(List.of(
            new byte[] {0x5a, 0x4d, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
        assertThrows(ZLinkServiceWireException.class, () -> codec.decode(List.of(
            new byte[] {0x5a, 0x4d, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0})));
    }

    @Test
    void decodedFramesDoNotAliasRawReceiveStorage() {
        byte[] raw = new byte[] {0x5a, 0x4d, 1, 1, 0, 1, 2, 3};
        ZLinkServiceWireFrame decoded = codec.decode(List.of(raw));
        raw[5] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, decoded.frames().getFirst());
    }

    @Test
    void decodesSharedLivenessFixturesDirectly() throws Exception {
        Path fixturePath = sharedFixturePath();
        JsonNode fixture = new ObjectMapper().readTree(Files.readString(fixturePath));

        for (JsonNode canonical : fixture.path("canonical")) {
            byte[] bytes = bytes(canonical.path("bytes"));
            assertEquals(13, bytes.length);
            ZLinkServiceWireFrame decoded = codec.decode(List.of(bytes));
            assertEquals(canonical.path("commandId").asInt(), decoded.command());
            assertArrayEquals(
                new byte[] {1, 2, 3, 4, 5, 6, 7, 8},
                decoded.frames().getFirst());
        }
        for (JsonNode malformed : fixture.path("malformed")) {
            byte[] bytes = bytes(malformed.path("bytes"));
            assertThrows(ZLinkServiceWireException.class, () -> codec.decode(List.of(bytes)));
        }
    }

    private static byte[] bytes(JsonNode values) {
        byte[] result = new byte[values.size()];
        for (int index = 0; index < values.size(); index++) {
            result[index] = (byte) values.get(index).asInt();
        }
        return result;
    }

    private static Path sharedFixturePath() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/service-decoder-fixtures-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared service decoder fixture was not found");
    }
}
