package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.HexFormat;

final class ZLinkSessionDisconnectedTest {
    @Test
    void commonGoldenVectorsRoundTripAndRejectMalformedBodies() throws Exception {
        var golden =
                new ObjectMapper()
                        .readTree(getClass().getResourceAsStream("/session-disconnected-v1.json"));
        assertEquals(2, golden.path("canonical").size());
        assertEquals(5, golden.path("rejected").size());
        for (var entry : golden.path("canonical")) {
            byte[] bytes = HexFormat.of().parseHex(entry.path("hex").asText());
            var identity = ZLinkSessionDisconnected.decode(bytes);
            assertEquals(
                    RoutingId.from(
                            HexFormat.of().parseHex(entry.path("sessionOwnerNodeRidHex").asText())),
                    identity.sessionOwnerNodeRid());
            assertEquals(
                    Long.parseUnsignedLong(entry.path("sessionOwnerNodeGeneration").asText()),
                    identity.sessionOwnerNodeGeneration());
            assertEquals(
                    Long.parseUnsignedLong(entry.path("bindingGeneration").asText()),
                    identity.bindingGeneration());
            assertArrayEquals(bytes, identity.encode());
            for (int offset :
                    new int[] {bytes.length - Long.BYTES * 2, bytes.length - Long.BYTES}) {
                byte[] zeroGeneration = bytes.clone();
                java.util.Arrays.fill(zeroGeneration, offset, offset + Long.BYTES, (byte) 0);
                assertEquals(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        assertThrows(
                                        ZLinkFrameworkException.class,
                                        () -> ZLinkSessionDisconnected.decode(zeroGeneration))
                                .kind());
            }
        }
        for (var entry : golden.path("rejected")) {
            byte[] bytes = HexFormat.of().parseHex(entry.path("hex").asText());
            assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    assertThrows(
                                    ZLinkFrameworkException.class,
                                    () -> ZLinkSessionDisconnected.decode(bytes),
                                    entry.path("name").asText())
                            .kind());
        }
    }
}
