package systems.zlink.e2e.pubsub.client.Support;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String mode,
    String publisherHttp,
    String publisherEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String sub1Http,
    String sub2Http,
    String sub3Http,
    String publisherReadyFile,
    String prelateContinueFile,
    String lateReadyFile,
    String lateContinueFile,
    String buildDir,
    String logDir,
    String configDir) {
    public static ClientOptions load(String[] args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: pub-sub-client --config <path> --scenario <selector>");
        }
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load PubSub client config", error);
        }
        return new ClientOptions(
            args[3],
            required(values, "publisherHttp"),
            required(values, "publisherEndpoint"),
            required(values, "redisLocationEndpoint"),
            required(values, "locationKeyPrefix"),
            required(values, "sub1Http"),
            required(values, "sub2Http"),
            required(values, "sub3Http"),
            required(values, "publisherReadyFile"),
            required(values, "prelateContinueFile"),
            required(values, "lateReadyFile"),
            required(values, "lateContinueFile"),
            required(values, "buildDir"),
            required(values, "logDir"),
            required(values, "configDir"));
    }

    public String subscriberHttp(String rid) {
        return switch (rid) {
            case "sub-1" -> sub1Http;
            case "sub-2" -> sub2Http;
            case "sub-3" -> sub3Http;
            default -> throw new IllegalArgumentException("unknown subscriber " + rid);
        };
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required in PubSub client config");
        }
        return value;
    }
}
