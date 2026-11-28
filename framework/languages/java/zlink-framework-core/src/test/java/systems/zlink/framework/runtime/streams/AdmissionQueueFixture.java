package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.params.provider.Arguments;

import systems.zlink.framework.testsupport.ZLinkConformanceFixtures;

import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/** Reads one group of the shared login admission queue fixture. */
final class AdmissionQueueFixture {
    static final String KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

    private AdmissionQueueFixture() {}

    static Stream<Arguments> scenarios(String group, int expectedCount) throws Exception {
        var fixture = ZLinkConformanceFixtures.file("login-admission-queue-v1.json");
        var scenarios = new ObjectMapper().readTree(fixture.toFile()).required(group);
        assertEquals(expectedCount, scenarios.size());
        return StreamSupport.stream(scenarios.spliterator(), false)
                .map(scenario -> Arguments.of(scenario.required("name").asText(), scenario));
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
