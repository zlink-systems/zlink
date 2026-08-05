package systems.zlink.e2e.spotactortransfer.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String nodeAHttpEndpoint, String nodeBHttpEndpoint, String nodeCHttpEndpoint,
    String streamAEndpoint, String streamBEndpoint, String streamCEndpoint,
    String logDirectory) {
    public ClientOptions {
        required(nodeAHttpEndpoint, "nodeAHttpEndpoint"); required(nodeBHttpEndpoint, "nodeBHttpEndpoint");
        required(nodeCHttpEndpoint, "nodeCHttpEndpoint"); required(streamAEndpoint, "streamAEndpoint");
        required(streamBEndpoint, "streamBEndpoint"); required(streamCEndpoint, "streamCEndpoint");
        required(logDirectory, "logDirectory");
    }
    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) { values.load(reader); }
        catch (Exception error) { throw new IllegalStateException("Could not load SpotActorTransfer client config", error); }
        return new ClientOptions(required(values, "nodeAHttpEndpoint"), required(values, "nodeBHttpEndpoint"),
            required(values, "nodeCHttpEndpoint"), required(values, "streamAEndpoint"),
            required(values, "streamBEndpoint"), required(values, "streamCEndpoint"),
            required(values, "logDirectory"));
    }
    private static String required(Properties values, String name) { String value = values.getProperty(name); required(value, name); return value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
    }
}
