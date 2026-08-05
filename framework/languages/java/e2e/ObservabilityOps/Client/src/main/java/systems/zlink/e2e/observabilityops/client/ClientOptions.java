package systems.zlink.e2e.observabilityops.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String streamEndpoint,
    String playHttpEndpoint,
    String playBHttpEndpoint,
    String sessionHttpEndpoint,
    String scenarioOutput,
    String drainUrl) {
    public ClientOptions {
        required(streamEndpoint, "streamEndpoint");
        required(playHttpEndpoint, "playHttpEndpoint");
        required(playBHttpEndpoint, "playBHttpEndpoint");
        required(sessionHttpEndpoint, "sessionHttpEndpoint");
        scenarioOutput = optional(scenarioOutput);
        drainUrl = optional(drainUrl);
    }

    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load ObservabilityOps client config", error);
        }
        return new ClientOptions(
            required(values, "streamEndpoint"),
            required(values, "playHttpEndpoint"),
            required(values, "playBHttpEndpoint"),
            required(values, "sessionHttpEndpoint"),
            values.getProperty("scenarioOutput", ""),
            values.getProperty("drainUrl", ""));
    }

    public Path requiredScenarioOutput() {
        required(scenarioOutput, "scenarioOutput");
        return Path.of(scenarioOutput);
    }

    public String requiredDrainUrl() {
        required(drainUrl, "drainUrl");
        return drainUrl;
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        required(value, name);
        return value;
    }

    private static String optional(String value) {
        return value == null ? "" : value;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
    }
}
