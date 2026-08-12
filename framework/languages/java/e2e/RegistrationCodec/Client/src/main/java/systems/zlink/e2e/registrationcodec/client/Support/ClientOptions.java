package systems.zlink.e2e.registrationcodec.client.Support;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String serverEndpoint,
    String httpEndpoint,
    String codecRequesterHttpEndpoint,
    String invalidServerEndpoint,
    String scenario,
    String buildDir,
    String logDir,
    String invalidServerConfig) {
    public static ClientOptions load(String[] args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException("Usage: registration-codec-client --config <path> --scenario <selector>");
        }
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load RegistrationCodec client config", error);
        }
        return new ClientOptions(
            required(values, "serverEndpoint"),
            required(values, "httpEndpoint"),
            required(values, "codecRequesterHttpEndpoint"),
            required(values, "invalidServerEndpoint"),
            args[3],
            required(values, "buildDir"),
            required(values, "logDir"),
            required(values, "invalidServerConfig"));
    }
    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }
}
