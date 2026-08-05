package systems.zlink.e2e.runtimemonitoring.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String triggerHttpEndpoint, String serviceHttpEndpoint, String serviceBHttpEndpoint,
    String handshakeEndpoint, String filteredServiceBinary,
    String filteredServiceConfigPath, String redisContainer, String logDirectory) {
    public ClientOptions {
        required(triggerHttpEndpoint, "triggerHttpEndpoint"); required(serviceHttpEndpoint, "serviceHttpEndpoint");
        required(serviceBHttpEndpoint, "serviceBHttpEndpoint"); required(handshakeEndpoint, "handshakeEndpoint");
        required(filteredServiceBinary, "filteredServiceBinary"); required(filteredServiceConfigPath, "filteredServiceConfigPath");
        required(redisContainer, "redisContainer");
        required(logDirectory, "logDirectory");
    }
    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) { values.load(reader); }
        catch (Exception error) { throw new IllegalStateException("Could not load RuntimeMonitoring client config", error); }
        return new ClientOptions(required(values, "triggerHttpEndpoint"), required(values, "serviceHttpEndpoint"),
            required(values, "serviceBHttpEndpoint"), required(values, "handshakeEndpoint"),
            required(values, "filteredServiceBinary"), required(values, "filteredServiceConfigPath"),
            required(values, "redisContainer"),
            required(values, "logDirectory"));
    }
    private static String required(Properties values, String name) { String value = values.getProperty(name); required(value, name); return value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
    }
}
