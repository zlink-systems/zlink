package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.params.provider.Arguments;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/** Reads one group of the shared login admission queue fixture. */
final class AdmissionQueueFixture {
    static final String KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

    private AdmissionQueueFixture() {}

    static Stream<Arguments> scenarios(String group, int expectedCount) throws Exception {
        for (Path directory = Path.of("").toAbsolutePath();
                directory != null;
                directory = directory.getParent()) {
            var fixture =
                    directory.resolve(
                            "framework/runtime/conformance/login-admission-queue-v1.json");
            if (Files.isRegularFile(fixture)) {
                var scenarios = new ObjectMapper().readTree(fixture.toFile()).required(group);
                assertEquals(expectedCount, scenarios.size());
                return StreamSupport.stream(scenarios.spliterator(), false)
                        .map(
                                scenario ->
                                        Arguments.of(scenario.required("name").asText(), scenario));
            }
        }
        throw new AssertionError("login-admission-queue-v1.json was not found");
    }

    /** Fixture names are text; the runtime identities are their UTF-8 bytes, base64url. */
    static String id(String name) {
        return Base64.getUrlEncoder()
                .withoutPadding()
                .encodeToString(name.getBytes(StandardCharsets.UTF_8));
    }

    static String text(JsonNode node, String field, String fallback) {
        return node.has(field) ? node.get(field).asText() : fallback;
    }

    static long number(JsonNode node, String field, long fallback) {
        return node.has(field) ? node.get(field).asLong() : fallback;
    }
}
