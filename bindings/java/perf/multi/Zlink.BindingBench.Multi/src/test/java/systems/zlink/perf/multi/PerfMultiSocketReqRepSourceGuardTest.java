package systems.zlink.perf.multi;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;

class PerfMultiSocketReqRepSourceGuardTest {
    private static final Path SOURCE = Path.of("src", "main", "java",
        "systems", "zlink", "perf", "multi", "PerfMultiSocketReqRep.java");

    @Test
    void requestHotPathKeepsReusableNativeTemplatesAndNoByteArrayRoundTrip()
        throws IOException {
        String source = Files.readString(SOURCE, StandardCharsets.UTF_8);

        assertTrue(source.contains("class RequestPayloadTemplates"));
        assertTrue(source.contains("Message.from(templates[index])"));
        assertFalse(source.contains("toByteArray()"));
        assertFalse(source.contains("byte[][] pendingPayloads"));
    }
}
