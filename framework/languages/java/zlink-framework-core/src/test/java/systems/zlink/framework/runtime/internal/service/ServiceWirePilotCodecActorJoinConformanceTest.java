package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

/**
 * Pins command-28 to the generated canonical codec and the shared golden
 * request vectors.
 */
final class ServiceWirePilotCodecActorJoinConformanceTest {
    @Test
    void generatedEncoderMatchesEveryCanonicalGoldenVector() throws Exception {
        JsonNode fixture = fixture();
        for (JsonNode entry : fixture.path("valid")) {
            List<byte[]> expected = frames(entry.path("framesHex"));
            List<byte[]> actual = ServiceWirePilotCodec.encodeActorJoin28(
                actorJoin(entry.path("input")));
            assertEquals(expected.size(), actual.size(), entry.path("name").asText());
            for (int index = 0; index < expected.size(); index++) {
                assertArrayEquals(expected.get(index), actual.get(index),
                    entry.path("name").asText() + " frame " + index);
            }
        }
    }

    @Test
    void generatedDecoderRejectsEveryMalformedGoldenVector() throws Exception {
        JsonNode fixture = fixture();
        for (JsonNode entry : fixture.path("invalid")) {
            IOException failure = assertThrows(IOException.class,
                () -> ServiceWirePilotCodec.decodeActorJoin28(
                    frames(entry.path("framesHex"))),
                entry.path("name").asText());
            assertEquals(entry.path("error").asText(), failure.getMessage(),
                entry.path("name").asText());
        }
    }

    private static ServiceWirePilotCodec.ActorJoin28 actorJoin(JsonNode input) {
        return new ServiceWirePilotCodec.ActorJoin28(
            Long.parseUnsignedLong(input.path("correlation").asText()),
            fence(input.path("actor")),
            input.path("entry").asBoolean(),
            fence(input.path("targetSpot")),
            input.has("payload") ? payload(input.path("payload")) : null);
    }

    private static ServiceWirePilotCodec.Fence fence(JsonNode input) {
        return new ServiceWirePilotCodec.Fence(
            input.path("id").asText(),
            Long.parseUnsignedLong(input.path("generation").asText()),
            HexFormat.of().parseHex(input.path("targetNodeRidHex").asText()),
            Long.parseUnsignedLong(input.path("targetNodeGeneration").asText()),
            Long.parseUnsignedLong(
                input.path("expectedAuthorityOwnerGeneration").asText()),
            Long.parseUnsignedLong(
                input.path("expectedOwnerLeaseGeneration").asText()));
    }

    private static ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1 payload(
        JsonNode input) {
        return new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
            input.path("packetName").asText(),
            input.path("contentType").asText(),
            HexFormat.of().parseHex(input.path("payloadHex").asText()));
    }

    private static List<byte[]> frames(JsonNode hexFrames) {
        List<byte[]> frames = new ArrayList<>();
        for (JsonNode frame : hexFrames) {
            frames.add(HexFormat.of().parseHex(frame.asText()));
        }
        return frames;
    }

    private static JsonNode fixture() throws IOException {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/actor-join-request-v1.json");
            if (Files.isRegularFile(candidate)) return candidate;
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared actor join request fixture was not found");
    }
}
