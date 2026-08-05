package systems.zlink.e2e.storefailure.client.support;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;

public record ClientOptions(
    List<String> expectedRids,
    String consumerHttpEndpoint,
    String deadRid,
    List<String> expectedAbsentRids,
    long locationHeartbeatMillis,
    long locationLeaseTtlMillis,
    long locationPollingMillis,
    long locationStoreFailureGraceMillis) {

    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load StoreFailure client config", error);
        }
        List<String> expected = csv(values, "expectedRids");
        if (expected.isEmpty()) throw new IllegalArgumentException("expectedRids is required");
        return new ClientOptions(
            expected,
            required(values, "consumerHttpEndpoint"),
            values.getProperty("deadRid", "api-b"),
            csv(values, "expectedAbsentRids"),
            positive(values, "locationHeartbeatMillis"),
            positive(values, "locationLeaseTtlMillis"),
            positive(values, "locationPollingMillis"),
            positive(values, "locationStoreFailureGraceMillis"));
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }

    private static long positive(Properties values, String name) {
        long value = Long.parseLong(required(values, name));
        if (value <= 0) throw new IllegalArgumentException(name + " must be positive");
        return value;
    }

    private static List<String> csv(Properties values, String name) {
        String value = values.getProperty(name, "");
        return Arrays.stream(value.split(","))
            .map(String::trim).filter(part -> !part.isBlank()).toList();
    }
}
