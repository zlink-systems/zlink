package systems.zlink.samples.gamequest.client;

import java.io.IOException;
import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record GameQuestClientOptions(
    String apiAStreamEndpoint,
    String apiBStreamEndpoint,
    String apiAHttpEndpoint,
    String apiBHttpEndpoint,
    String scenario) {

    public static GameQuestClientOptions load(String[] args) throws IOException {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: Client --config <path>");
        }
        Properties properties = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            properties.load(reader);
        }
        return new GameQuestClientOptions(
            required(properties, "sample.apiAStreamEndpoint"),
            required(properties, "sample.apiBStreamEndpoint"),
            required(properties, "sample.apiAHttpEndpoint"),
            required(properties, "sample.apiBHttpEndpoint"),
            properties.getProperty("sample.scenario", "full"));
    }

    private static String required(Properties properties, String name) {
        String value = properties.getProperty(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
        return value;
    }
}
