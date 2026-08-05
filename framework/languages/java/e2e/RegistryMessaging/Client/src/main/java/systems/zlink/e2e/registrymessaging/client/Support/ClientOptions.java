package systems.zlink.e2e.registrymessaging.client.Support;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String providerAHttpUrl,
    String providerBHttpUrl,
    String workflowHttpUrl,
    String discoveryConsumerHttpUrl,
    String directConsumerHttpUrl,
    String singleConsumerHttpUrl,
    String backpressureConsumerHttpUrl,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String buildDir,
    String logDir,
    String configDir) {
    public ClientOptions {
        required(providerAHttpUrl, "providerAHttpUrl");
        required(providerBHttpUrl, "providerBHttpUrl");
        required(workflowHttpUrl, "workflowHttpUrl");
        required(discoveryConsumerHttpUrl, "discoveryConsumerHttpUrl");
        required(directConsumerHttpUrl, "directConsumerHttpUrl");
        required(singleConsumerHttpUrl, "singleConsumerHttpUrl");
        required(backpressureConsumerHttpUrl, "backpressureConsumerHttpUrl");
        required(redisLocationEndpoint, "redisLocationEndpoint");
        required(locationKeyPrefix, "locationKeyPrefix");
        required(buildDir, "buildDir");
        required(logDir, "logDir");
        required(configDir, "configDir");
    }

    public static ClientOptions load(String path) {
        Properties values = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            values.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load RegistryMessaging client config", error);
        }
        return new ClientOptions(
            required(values, "providerAHttpUrl"), required(values, "providerBHttpUrl"),
            required(values, "workflowHttpUrl"), required(values, "discoveryConsumerHttpUrl"),
            required(values, "directConsumerHttpUrl"), required(values, "singleConsumerHttpUrl"),
            required(values, "backpressureConsumerHttpUrl"), required(values, "redisLocationEndpoint"),
            required(values, "locationKeyPrefix"), required(values, "buildDir"),
            required(values, "logDir"), required(values, "configDir"));
    }

    private static String required(Properties values, String name) {
        String value = values.getProperty(name);
        required(value, name);
        return value;
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
    }
}
