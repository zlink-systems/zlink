package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.streams.ZLinkSession;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

final class ZLinkAdmissionQueueStartupConformanceTest {
    static Stream<Arguments> startupScenarios() throws Exception {
        for (Path directory = Path.of("").toAbsolutePath();
                directory != null;
                directory = directory.getParent()) {
            var fixture =
                    directory.resolve(
                            "framework/runtime/conformance/login-admission-queue-v1.json");
            if (Files.isRegularFile(fixture)) {
                var scenarios =
                        new ObjectMapper().readTree(fixture.toFile()).required("startupScenarios");
                assertEquals(7, scenarios.size());
                return StreamSupport.stream(scenarios.spliterator(), false)
                        .map(
                                scenario ->
                                        Arguments.of(scenario.required("name").asText(), scenario));
            }
        }
        throw new AssertionError("login-admission-queue-v1.json was not found");
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("startupScenarios")
    void validatesQueueMeshAtStartup(String name, JsonNode scenario) {
        var given = scenario.required("given");
        var expect = scenario.required("expect");
        var options = new DefaultZLinkFrameworkOptions();
        for (var mesh : given.required("meshNodes")) {
            options.addRouteMesh(mesh.asText()).listen(0);
        }
        var builder =
                options.addStreamNode(streamName(given))
                        .bind(0)
                        .registerSession(LoginSession.class)
                        .authenticationPacketName("Login")
                        .admissionQueueEnabled(given.required("queueEnabled").asBoolean())
                        .admissionQueueKey(
                                Base64.getUrlEncoder()
                                        .withoutPadding()
                                        .encodeToString(new byte[32]));
        if (given.get("queueMeshName").isTextual()) {
            builder.admissionQueueMeshName(given.get("queueMeshName").asText());
        }
        if (expect.required("started").asBoolean()) {
            assertDoesNotThrow(options::validate);
            var registration = options.registration();
            var queue =
                    ZLinkStreamAuthenticationAdmission.settings(
                                    registration.streamNodes(), registration.meshNodes())
                            .queue();
            if (expect.get("queueMesh").isNull()) {
                assertNull(queue);
            } else {
                assertEquals(expect.required("queueMesh").asText(), queue.meshName());
                assertEquals(expect.required("queueSpotId").asText(), queue.spotId());
            }
        } else {
            var error = assertThrows(ZLinkConfigurationException.class, options::validate);
            if (expect.required("configurationError").isTextual()) {
                assertEquals(expect.get("configurationError").asText(), error.getMessage());
            }
        }
    }

    private static String streamName(JsonNode given) {
        return given.has("streamNameUtf8Bytes")
                ? "x".repeat(given.get("streamNameUtf8Bytes").asInt())
                : "login";
    }

    private abstract static class LoginSession implements ZLinkSession {}
}
