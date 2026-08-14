package systems.zlink.framework.runtime.internal.service;
import java.util.Arrays;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import org.junit.jupiter.api.Test;

final class ZLinkCanonicalRelocationControlCodecTest {
    @Test
    void sharedCanonicalControlsRoundTripByteForByte() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        var codec = new ZLinkCanonicalRelocationControlCodec();
        assertEquals(4, fixture.path("canonical").size());
        for (JsonNode entry : fixture.path("canonical")) {
            byte[] bytes = HexFormat.of().parseHex(entry.path("hex").asText());
            ZLinkCanonicalRelocationControlCodec.Control decoded;
            try {
                decoded = codec.decode(bytes);
            } catch (IllegalArgumentException failure) {
                throw new AssertionError(entry.path("name").asText()
                    + ": " + failure.getMessage(), failure);
            }
            assertEquals(entry.path("command").asInt(), decoded.command());
            assertArrayEquals(bytes, codec.encode(decoded));

            byte[] truncated = Arrays.copyOf(bytes, bytes.length - 1);
            assertThrows(IllegalArgumentException.class, () -> codec.decode(truncated));
        }
    }

    @Test
    void reservedCommandsAndWrongSenderRoleAreRejected() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        var codec = new ZLinkCanonicalRelocationControlCodec();
        byte[] unknown = HexFormat.of().parseHex(
            fixture.path("canonical").get(2).path("hex").asText());
        unknown[3] = 39;
        assertThrows(IllegalArgumentException.class, () -> codec.decode(unknown));

        byte[] cutover = HexFormat.of().parseHex(
            fixture.path("canonical").get(2).path("hex").asText());
        for (int reserved : new int[]{32, 35, 41}) {
            byte[] frame = cutover.clone();
            frame[3] = (byte) reserved;
            assertThrows(IllegalArgumentException.class,
                () -> codec.decode(frame));
        }

        byte[] ready = HexFormat.of().parseHex(
            fixture.path("canonical").get(0).path("hex").asText());
        ready[ready.length - 1] = 1;
        assertThrows(IllegalArgumentException.class, () -> codec.decode(ready));
    }

    private static Path fixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/relocation-control-v1.json");
            if (Files.isRegularFile(candidate)) return candidate;
            current = current.getParent();
        }
        throw new IllegalStateException("shared relocation fixture was not found");
    }
}
