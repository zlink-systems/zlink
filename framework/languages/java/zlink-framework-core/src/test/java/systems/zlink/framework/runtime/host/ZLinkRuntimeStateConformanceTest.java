package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkRuntimeStateConformanceTest {
    @Test
    void publicStateFixtureIsTheSingleReadinessAuthority() throws Exception {
        JsonNode fixture = fixture();
        assertEquals("zlink.framework.runtime-state", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        assertTrue(
            fixture.path("authorityInvariants")
                .path("publicStateIsSingleReadinessAuthority").asBoolean());
        assertFalse(
            fixture.path("authorityInvariants")
                .path("independentMutableReadinessAuthority").asBoolean());

        int states = 0;
        for (JsonNode stateCase : fixture.path("publicStates")) {
            ZLinkFrameworkRuntimeState state = ZLinkFrameworkRuntimeState.valueOf(
                stateCase.path("name").asText().toUpperCase(Locale.ROOT));
            assertEquals(stateCase.path("wireValue").asInt(), state.wireValue());
            assertEquals(stateCase.path("isReady").asBoolean(), state.isReadyState());
            states++;
        }
        assertEquals(ZLinkFrameworkRuntimeState.values().length, states);

        for (JsonNode scenario : fixture.path("acceptingWorkScenarios")) {
            ZLinkFrameworkRuntimeState state = ZLinkFrameworkRuntimeState.valueOf(
                scenario.path("state").asText().toUpperCase(Locale.ROOT));
            assertEquals(
                scenario.path("expected").asBoolean(),
                state.acceptsWork(scenario.path("admissionOpen").asBoolean()),
                scenario.toString());
        }
    }

    @Test
    void startupAndShutdownNeverPublishAContradictoryReadinessProjection()
        throws Exception {
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            new DefaultZLinkFrameworkOptions(),
            new ZLinkJavaBackendAdapterFactory());
        try {
            long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
            while (!runtime.isReady() && System.nanoTime() < deadline) {
                assertProjection(runtime);
                Thread.sleep(1);
            }
            assertTrue(runtime.isReady());
            assertProjection(runtime);
            assertTrue(runtime.status().acceptingWork());

            var closing = runtime.closeAsync();
            assertProjection(runtime);
            assertFalse(runtime.isReady());
            closing.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertProjection(runtime);
            assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
        } finally {
            runtime.close();
        }
    }

    private static void assertProjection(ZLinkFrameworkRuntime runtime) {
        var status = runtime.status();
        boolean expectedReady = status.state() == ZLinkFrameworkRuntimeState.SERVING;
        assertEquals(expectedReady, status.isReady());
        assertEquals(expectedReady, runtime.isReady());
        assertEquals(
            status.state().acceptsWork(status.acceptingWork()),
            status.acceptingWork());
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/conformance/runtime-state-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared runtime state fixture was not found");
    }
}
