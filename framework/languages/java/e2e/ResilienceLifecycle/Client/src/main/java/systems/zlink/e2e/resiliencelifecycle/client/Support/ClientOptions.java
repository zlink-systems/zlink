package systems.zlink.e2e.resiliencelifecycle.client.Support;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String apiAEndpoint,
    String apiBEndpoint,
    String apiAReplacementEndpoint,
    String apiBGreenEndpoint,
    String httpAEndpoint,
    String httpBEndpoint,
    String httpAReplacementEndpoint,
    String httpBGreenEndpoint,
    String storePauseCommand,
    String storeResumeCommand,
    String buildDir,
    String logDir,
    String controlDir,
    String configDir) {
    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load ResilienceLifecycle client config", error);
        }
        return new ClientOptions(
            required(values, "redisLocationEndpoint"), required(values, "locationKeyPrefix"),
            required(values, "apiAEndpoint"), required(values, "apiBEndpoint"),
            required(values, "apiAReplacementEndpoint"), required(values, "apiBGreenEndpoint"),
            required(values, "httpAEndpoint"), required(values, "httpBEndpoint"),
            required(values, "httpAReplacementEndpoint"), required(values, "httpBGreenEndpoint"),
            required(values, "storePauseCommand"), required(values, "storeResumeCommand"),
            required(values, "buildDir"), required(values, "logDir"),
            required(values, "controlDir"), required(values, "configDir"));
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }
}
