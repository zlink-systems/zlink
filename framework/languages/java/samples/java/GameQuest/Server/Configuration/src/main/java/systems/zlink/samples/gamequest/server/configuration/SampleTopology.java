package systems.zlink.samples.gamequest.server.configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record SampleTopology(
    String instanceName,
    String logDirectory,
    String streamEndpoint,
    String spotRouterEndpoint,
    String httpEndpoint,
    String redisEndpoint,
    String redisKeyPrefix) {

    public GameApi gameApi() {
        return new GameApi(
            required(instanceName, "instanceName"),
            required(logDirectory, "logDirectory"),
            required(streamEndpoint, "streamEndpoint"),
            required(httpEndpoint, "httpEndpoint"),
            required(spotRouterEndpoint, "spotRouterEndpoint"));
    }

    public QuestMission questMission() {
        return new QuestMission(
            required(instanceName, "instanceName"),
            required(logDirectory, "logDirectory"),
            required(httpEndpoint, "httpEndpoint"),
            required(spotRouterEndpoint, "spotRouterEndpoint"));
    }

    public Location location() {
        return new Location(
            required(redisEndpoint, "redisEndpoint"),
            required(redisKeyPrefix, "redisKeyPrefix"));
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

    public record GameApi(
        String instanceName,
        String logDirectory,
        String streamEndpoint,
        String httpEndpoint,
        String spotRouterEndpoint) {
    }

    public record QuestMission(
        String instanceName,
        String logDirectory,
        String httpEndpoint,
        String spotRouterEndpoint) {
    }

    public record Location(String redisEndpoint, String redisKeyPrefix) {
    }
}
