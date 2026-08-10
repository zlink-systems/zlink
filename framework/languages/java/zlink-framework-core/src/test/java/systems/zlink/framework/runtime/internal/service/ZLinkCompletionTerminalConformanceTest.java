package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkCompletionTerminalConformanceTest {
    @Test
    void consumesSharedCapacityIdentityAndWireSeparation() throws Exception {
        JsonNode fixture = fixture();
        assertEquals("zlink.framework.completion-terminal", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        assertEquals(
            ZLinkServiceOperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS,
            fixture.path("limits").path("pendingOperationCapacity").asInt());
        assertEquals(128, fixture.path("limits").path("operationIdBits").asInt());
        assertEquals(64, fixture.path("limits").path("replyRouteIdBits").asInt());

        ZLinkServiceM6BWireCodec codec = new ZLinkServiceM6BWireCodec();
        for (JsonNode operation : fixture.path("operations")) {
            long high = Long.parseUnsignedLong(
                operation.path("operationId").path("high").asText());
            long low = Long.parseUnsignedLong(
                operation.path("operationId").path("low").asText());
            long replyRoute = Long.parseUnsignedLong(
                operation.path("replyRouteId").asText());
            assertTrue(high != 0 || low != 0);
            assertNotEquals(0, replyRoute);

            ZLinkServiceM6BWireCodec.InstanceSpotMessage expected =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                    0,
                    new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                        RoutingId.from("target"), 2, "spot", 3,
                        "owner", 4, 5, "version"),
                    "stable.Type",
                    6,
                    RoutingId.from("source"),
                    "source-spot",
                    true,
                    high,
                    low,
                    replyRoute);
            ZLinkServiceM6BWireCodec.InstanceSpotMessage decoded =
                codec.decodeInstanceSpotHeader(codec.encodeInstanceSpotHeader(expected));
            assertEquals(high, decoded.operationHigh());
            assertEquals(low, decoded.operationLow());
            assertEquals(replyRoute, decoded.replyRouteId());
        }
    }

    @Test
    void everySharedRaceRequiresOneApplicationTerminal() throws Exception {
        JsonNode fixture = fixture();
        for (JsonNode scenario : fixture.path("raceScenarios")) {
            assertTrue(
                scenario.path("applicationCompletionCount").asInt() <= 1,
                scenario.path("name").asText());
            if (!scenario.path("applicationTerminal").isNull()) {
                assertEquals(
                    1,
                    scenario.path("applicationCompletionCount").asInt(),
                    scenario.path("name").asText());
            }
        }
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/conformance/completion-terminal-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared completion terminal fixture was not found");
    }
}
