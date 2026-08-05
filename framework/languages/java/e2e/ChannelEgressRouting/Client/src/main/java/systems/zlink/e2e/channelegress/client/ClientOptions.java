package systems.zlink.e2e.channelegress.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String sessionEndpoint,
    String playEndpoint,
    String auditEndpoint,
    String workflowAEndpoint,
    String workflowBEndpoint,
    String apiAEndpoint,
    String apiBEndpoint,
    String callerEndpoint) {
    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("could not load ChannelEgressRouting client config", error);
        }
        return new ClientOptions(
            required(values, "sessionEndpoint"),
            required(values, "playEndpoint"),
            required(values, "auditEndpoint"),
            required(values, "workflowAEndpoint"),
            required(values, "workflowBEndpoint"),
            required(values, "apiAEndpoint"),
            required(values, "apiBEndpoint"),
            required(values, "callerEndpoint"));
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
        return value;
    }
}
