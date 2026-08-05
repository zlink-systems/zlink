package systems.zlink.samples.deliverydispatch.client;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record ClientOptions(
    String customerStreamEndpoint,
    String courierStreamEndpoint,
    String dispatchHttpEndpoint) {

    public static ClientOptions load(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: Client --config <path>");
        }
        Properties properties = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            properties.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load DeliveryDispatch client config.", error);
        }
        return new ClientOptions(
            required(properties, "customerStreamEndpoint"),
            required(properties, "courierStreamEndpoint"),
            required(properties, "dispatchHttpEndpoint"));
    }

    private static String required(Properties properties, String name) {
        String value = properties.getProperty(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing DeliveryDispatch client config: " + name);
        }
        return value;
    }
}
