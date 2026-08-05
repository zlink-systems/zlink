package systems.zlink.e2e.automaticturn.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String streamEndpoint, String playHttpEndpoint, String playBHttpEndpoint,
    String sessionHttpEndpoint, String shutdownRequestId, String shutdownSpotRid) {
    public ClientOptions {
        required(streamEndpoint, "streamEndpoint"); required(playHttpEndpoint, "playHttpEndpoint");
        required(playBHttpEndpoint, "playBHttpEndpoint"); required(sessionHttpEndpoint, "sessionHttpEndpoint");
        shutdownRequestId = optional(shutdownRequestId); shutdownSpotRid = optional(shutdownSpotRid);
    }
    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) { values.load(reader); }
        catch (Exception error) { throw new IllegalStateException("Could not load AutomaticTurnDispatch client config", error); }
        return new ClientOptions(required(values, "streamEndpoint"), required(values, "playHttpEndpoint"),
            required(values, "playBHttpEndpoint"), required(values, "sessionHttpEndpoint"),
            values.getProperty("shutdownRequestId", ""), values.getProperty("shutdownSpotRid", ""));
    }
    public String requiredShutdownRequestId() { required(shutdownRequestId, "shutdownRequestId"); return shutdownRequestId; }
    public String requiredShutdownSpotRid() { required(shutdownSpotRid, "shutdownSpotRid"); return shutdownSpotRid; }
    private static String required(Properties values, String name) { String value = values.getProperty(name); required(value, name); return value; }
    private static String optional(String value) { return value == null ? "" : value; }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
    }
}
