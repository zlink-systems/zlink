package systems.zlink.e2e.spotservice.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String gatewayHttpEndpoint,
    String streamAEndpoint,
    String streamBEndpoint,
    String tlsStreamAEndpoint,
    String httpAEndpoint,
    String httpBEndpoint,
    String multiAHttpEndpoint,
    String multiBHttpEndpoint,
    String readyFile,
    String crashedFile,
    String failedFile,
    String restartedFile,
    String secondCrashReadyFile,
    String secondCrashedFile) {
    public ClientOptions {
        required(gatewayHttpEndpoint, "gatewayHttpEndpoint");
        required(streamAEndpoint, "streamAEndpoint");
        required(streamBEndpoint, "streamBEndpoint");
        tlsStreamAEndpoint = tlsStreamAEndpoint == null ? "" : tlsStreamAEndpoint;
        required(httpAEndpoint, "httpAEndpoint");
        required(httpBEndpoint, "httpBEndpoint");
        required(multiAHttpEndpoint, "multiAHttpEndpoint");
        required(multiBHttpEndpoint, "multiBHttpEndpoint");
        required(readyFile, "readyFile");
        required(crashedFile, "crashedFile");
        required(failedFile, "failedFile");
        required(restartedFile, "restartedFile");
        required(secondCrashReadyFile, "secondCrashReadyFile");
        required(secondCrashedFile, "secondCrashedFile");
    }

    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load SpotService client config", error);
        }
        return new ClientOptions(
            required(values, "gatewayHttpEndpoint"), required(values, "streamAEndpoint"),
            required(values, "streamBEndpoint"), values.getProperty("tlsStreamAEndpoint", ""),
            required(values, "httpAEndpoint"), required(values, "httpBEndpoint"),
            required(values, "multiAHttpEndpoint"), required(values, "multiBHttpEndpoint"),
            required(values, "readyFile"), required(values, "crashedFile"),
            required(values, "failedFile"), required(values, "restartedFile"),
            required(values, "secondCrashReadyFile"), required(values, "secondCrashedFile"));
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        required(value, name);
        return value;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
    }
}
