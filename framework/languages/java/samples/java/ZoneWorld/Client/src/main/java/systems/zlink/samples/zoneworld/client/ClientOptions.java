package systems.zlink.samples.zoneworld.client;

import java.io.IOException;
import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Objects;
import java.util.Properties;
public record ClientOptions(String gatewayEndpoint, String opsEndpoint, String scenario) {
    public static ClientOptions load(String[] args) throws IOException {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: ZoneWorldClient --config <path>");
        }
        Properties properties = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            properties.load(reader);
        }
        return new ClientOptions(
            require(properties, "sample.gateway-endpoint"),
            require(properties, "sample.ops-endpoint"),
            properties.getProperty("sample.scenario", "full"));
    }

    private static String require(Properties properties, String key) {
        String value = Objects.requireNonNullElse(properties.getProperty(key), "");
        if (value.isBlank()) throw new IllegalArgumentException(key + " is required");
        return value;
    }
}
