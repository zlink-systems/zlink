package systems.zlink.samples.bingo.server.configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record SampleTopology(
    String apiAChannelEndpoint,
    String apiBChannelEndpoint,
    String apiAMeshEndpoint,
    String apiBMeshEndpoint,
    String sessionARouterEndpoint,
    String sessionBRouterEndpoint,
    String playASpotRouterEndpoint,
    String playBSpotRouterEndpoint,
    String apiMatchmakingRouterEndpoint,
    String matchmakingRouterEndpoint,
    String sessionAStreamEndpoint,
    String sessionBStreamEndpoint,
    String redisEndpoint,
    String redisKeyPrefix,
    String apiNode,
    String playNode,
    String sessionNode,
    String logDirectory) {

    public SampleTopology {
        apiAChannelEndpoint = value(apiAChannelEndpoint, "tcp://127.0.0.1:47103");
        apiBChannelEndpoint = value(apiBChannelEndpoint, "tcp://127.0.0.1:47117");
        apiAMeshEndpoint = value(apiAMeshEndpoint, "tcp://127.0.0.1:47104");
        apiBMeshEndpoint = value(apiBMeshEndpoint, "tcp://127.0.0.1:47118");
        sessionARouterEndpoint = value(sessionARouterEndpoint, "tcp://127.0.0.1:47106");
        sessionBRouterEndpoint = value(sessionBRouterEndpoint, "tcp://127.0.0.1:47120");
        playASpotRouterEndpoint = value(playASpotRouterEndpoint, "tcp://127.0.0.1:47111");
        playBSpotRouterEndpoint = value(playBSpotRouterEndpoint, "tcp://127.0.0.1:47122");
        apiMatchmakingRouterEndpoint = value(
            apiMatchmakingRouterEndpoint, "tcp://127.0.0.1:47127");
        matchmakingRouterEndpoint = value(
            matchmakingRouterEndpoint, "tcp://127.0.0.1:47128");
        sessionAStreamEndpoint = value(sessionAStreamEndpoint, "tcp://127.0.0.1:47114");
        sessionBStreamEndpoint = value(sessionBStreamEndpoint, "tcp://127.0.0.1:47125");
        redisEndpoint = required(redisEndpoint, "redisEndpoint");
        redisKeyPrefix = value(redisKeyPrefix, "bingo:java:");
        apiNode = value(apiNode, "a");
        playNode = value(playNode, "a");
        sessionNode = value(sessionNode, "a");
        logDirectory = required(logDirectory, "logDirectory");
    }

    public String selectedApiChannelEndpoint() {
        return "b".equals(apiNode) ? apiBChannelEndpoint : apiAChannelEndpoint;
    }

    public String selectedApiMeshEndpoint() {
        return "b".equals(apiNode) ? apiBMeshEndpoint : apiAMeshEndpoint;
    }

    public String selectedPlaySpotRouterEndpoint() {
        return "b".equals(playNode) ? playBSpotRouterEndpoint : playASpotRouterEndpoint;
    }

    public String selectedSessionRouterEndpoint() {
        return "b".equals(sessionNode) ? sessionBRouterEndpoint : sessionARouterEndpoint;
    }

    public String selectedStreamEndpoint() {
        return "b".equals(sessionNode) ? sessionBStreamEndpoint : sessionAStreamEndpoint;
    }

    public static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: <role executable> --config <path>");
        }
        return args[1];
    }

    private static String value(String value, String fallback) {
        return value == null || value.isBlank() ? fallback : value;
    }

    private static String required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("sample." + name + " is required");
        }
        return value;
    }
}
