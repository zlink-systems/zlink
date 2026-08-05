package systems.zlink.framework.runtime.internal.service;

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
        assertEquals(7, fixture.path("canonical").size());
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

            byte[] truncated = java.util.Arrays.copyOf(bytes, bytes.length - 1);
            assertThrows(IllegalArgumentException.class, () -> codec.decode(truncated));
        }
    }

    @Test
    void closedCommandAndPhaseAreRejected() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        var codec = new ZLinkCanonicalRelocationControlCodec();
        byte[] unknown = HexFormat.of().parseHex(
            fixture.path("canonical").get(2).path("hex").asText());
        unknown[3] = 39;
        assertThrows(IllegalArgumentException.class, () -> codec.decode(unknown));

        byte[] data = HexFormat.of().parseHex(
            fixture.path("canonical").get(1).path("hex").asText());
        // The canonical command-31 fixture ends with phase, role, identity,
        // terminal and failure. Locate the relocationControl phase after the
        // zero reply route by decoding the stable suffix marker.
        int phase = find(data, new byte[]{0, 0, 0, 0, 4, 1});
        data[phase + 4] = 10;
        assertThrows(IllegalArgumentException.class, () -> codec.decode(data));
    }

    private static int find(byte[] source, byte[] needle) {
        for (int i = 0; i <= source.length - needle.length; i++) {
            boolean match = true;
            for (int j = 0; j < needle.length; j++) {
                if (source[i + j] != needle[j]) { match = false; break; }
            }
            if (match) return i;
        }
        throw new AssertionError("fixture suffix was not found");
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
