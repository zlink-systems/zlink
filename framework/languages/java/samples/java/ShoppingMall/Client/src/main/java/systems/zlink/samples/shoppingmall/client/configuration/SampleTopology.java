package systems.zlink.samples.shoppingmall.client.configuration;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public record SampleTopology(String apiAHttpUrl, String apiBHttpUrl) {
    public static SampleTopology load(String[] args) throws Exception {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: Client --config <path>");
        }
        Properties properties = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) { properties.load(reader); }
        return new SampleTopology(required(properties, "sample.apiAHttpUrl"),
            required(properties, "sample.apiBHttpUrl"));
    }

    private static String required(Properties properties, String name) {
        String value = properties.getProperty(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }
}
