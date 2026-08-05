package systems.zlink.samples.supportchat.server.configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record SampleTopology(
    String redisEndpoint,
    String redisKeyPrefix,
    String logDirectory,
    String apiChannelEndpoint,
    String apiHttpEndpoint,
    String supportChannelEndpoint,
    String sessionStreamEndpoint,
    String sessionSpotRouterEndpoint,
    String supportSpotRouterEndpoint,
    String supportHttpEndpoint) {

    public Location location() {
        return new Location(required(redisEndpoint, "redisEndpoint"), required(redisKeyPrefix, "redisKeyPrefix"));
    }

    public String requiredLogDirectory() {
        return required(logDirectory, "logDirectory");
    }

    public Api api() {
        return new Api(
            required(apiChannelEndpoint, "apiChannelEndpoint"),
            required(apiHttpEndpoint, "apiHttpEndpoint"));
    }

    public Session session() {
        return new Session(
            required(sessionStreamEndpoint, "sessionStreamEndpoint"),
            required(sessionSpotRouterEndpoint, "sessionSpotRouterEndpoint"));
    }

    public Support support() {
        return new Support(
            required(supportChannelEndpoint, "supportChannelEndpoint"),
            required(supportSpotRouterEndpoint, "supportSpotRouterEndpoint"),
            required(supportHttpEndpoint, "supportHttpEndpoint"));
    }

    public static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: <role executable> --config <path>");
        }
        return args[1];
    }

    private static String required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("sample." + name + " is required");
        }
        return value;
    }

    public record Location(String redisEndpoint, String redisKeyPrefix) {
    }

    public record Api(String channelEndpoint, String httpEndpoint) {
    }

    public record Session(
        String streamEndpoint,
        String routerEndpoint) {
    }

    public record Support(
        String channelEndpoint,
        String routerEndpoint,
        String httpEndpoint) {
    }
}
