package systems.zlink.samples.shoppingmall.server.configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record SampleTopology(
    String instanceName,
    String logDirectory,
    String httpUrl,
    String channelEndpoint,
    String spotEndpoint,
    String spotRouterEndpoint,
    String redisEndpoint,
    String redisKeyPrefix) {

    public Api api() {
        return new Api(required(instanceName, "instanceName"), required(logDirectory, "logDirectory"),
            required(httpUrl, "httpUrl"));
    }

    public Workflow workflow() {
        String name = required(instanceName, "instanceName");
        return new Workflow(name, required(logDirectory, "logDirectory"), required(httpUrl, "httpUrl"),
            required(channelEndpoint, "channelEndpoint"), required(spotEndpoint, "spotEndpoint"),
            required(spotRouterEndpoint, "spotRouterEndpoint"));
    }

    public Location location() {
        return new Location(required(redisEndpoint, "redisEndpoint"), required(redisKeyPrefix, "redisKeyPrefix"));
    }

    public static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: <role executable> --config <path>");
        }
        return args[1];
    }

    private static String required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("sample." + name + " is required");
        return value;
    }

    public record Api(String instanceName, String logDirectory, String httpUrl) { }
    public record Workflow(String instanceName, String logDirectory, String httpUrl, String channelEndpoint,
        String spotEndpoint, String spotRouterEndpoint) { }
    public record Location(String redisEndpoint, String redisKeyPrefix) { }
}
