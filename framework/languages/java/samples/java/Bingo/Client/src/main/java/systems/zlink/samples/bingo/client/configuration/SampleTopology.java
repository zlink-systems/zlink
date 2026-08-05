package systems.zlink.samples.bingo.client.configuration;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record SampleTopology(String sessionAStreamEndpoint, String sessionBStreamEndpoint) {
    public static SampleTopology load(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: Client --config <path>");
        }
        Properties properties = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            properties.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load Bingo client config.", error);
        }
        return new SampleTopology(
            required(properties, "sessionAStreamEndpoint"),
            required(properties, "sessionBStreamEndpoint"));
    }

    private static String required(Properties properties, String name) {
        String value = properties.getProperty(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing Bingo client config: " + name);
        }
        return value;
    }
}
