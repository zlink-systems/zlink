package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import org.junit.jupiter.api.Test;

/**
 * Golden-vector conformance for {@link ZLinkCanonicalActorJoinReplyCodec}
 * against the shared actor-join-reply-tail fixture
 * (framework/runtime/protocol/golden/actor-join-reply-v1.json), which is
 * consumed identically by node's decodeStatefulReply.
 */
final class ZLinkCanonicalActorJoinReplyCodecTest {
    @Test
    void sharedCanonicalVectorsRoundTripByteForByte() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        var codec = new ZLinkCanonicalActorJoinReplyCodec();
        assertEquals(5, fixture.path("canonical").size());
        for (JsonNode entry : fixture.path("canonical")) {
            String name = entry.path("name").asText();
            byte[] bytes = HexFormat.of().parseHex(entry.path("hex").asText());
            ZLinkCanonicalActorJoinReplyCodec.ActorJoinReply decoded;
            try {
                decoded = codec.decode(bytes);
            } catch (IllegalArgumentException failure) {
                throw new AssertionError(name + ": " + failure.getMessage(), failure);
            }
            JsonNode fields = entry.path("decoded");
            assertEquals(fields.path("joinResult").asText().equals("accepted") ? 0 : 1,
                decoded.joinResult(), name);
            assertEquals(Long.parseUnsignedLong(entry.path("correlation").asText()),
                decoded.correlation(), name);
            JsonNode spotField = fields.path("spot");
            if (spotField.isMissingNode()) {
                assertNull(decoded.spot(), name);
            } else {
                assertEquals(spotField.path("spotId").asText(), decoded.spot().spotId(), name);
                assertEquals(Long.parseUnsignedLong(spotField.path("generation").asText()),
                    decoded.spot().objectGeneration(), name);
            }
            if (fields.has("membershipEpoch")) {
                assertEquals(Long.parseUnsignedLong(fields.path("membershipEpoch").asText()),
                    decoded.membershipEpoch(), name);
            } else {
                assertNull(decoded.membershipEpoch(), name);
            }
            if (fields.has("receiveChunkLimitBytes")) {
                assertEquals(fields.path("receiveChunkLimitBytes").asLong(),
                    decoded.receiveChunkLimitBytes(), name);
            } else {
                assertNull(decoded.receiveChunkLimitBytes(), name);
            }
            assertArrayEquals(bytes, codec.encode(decoded), name);

            byte[] truncated = java.util.Arrays.copyOf(bytes, bytes.length - 1);
            assertThrows(IllegalArgumentException.class, () -> codec.decode(truncated), name);
        }
    }

    @Test
    void sharedMalformedVectorsAreRejected() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        var codec = new ZLinkCanonicalActorJoinReplyCodec();
        assertEquals(7, fixture.path("malformed").size());
        for (JsonNode entry : fixture.path("malformed")) {
            byte[] bytes = HexFormat.of().parseHex(entry.path("hex").asText());
            IllegalArgumentException failure = assertThrows(
                IllegalArgumentException.class, () -> codec.decode(bytes),
                entry.path("name").asText());
            assertTrue(failure.getMessage() != null && !failure.getMessage().isEmpty());
        }
    }

    private static Path fixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/actor-join-reply-v1.json");
            if (Files.isRegularFile(candidate)) return candidate;
            current = current.getParent();
        }
        throw new IllegalStateException("shared actor join reply fixture was not found");
    }
}
