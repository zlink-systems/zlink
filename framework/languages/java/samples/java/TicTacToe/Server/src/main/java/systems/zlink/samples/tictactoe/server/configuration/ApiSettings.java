package systems.zlink.samples.tictactoe.server.configuration;

import java.util.List;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record ApiSettings(
    String apiBindUrl,
    String apiChannelEndpoint,
    List<String> playEndpoints,
    String routeEndpoint,
    List<String> spotEndpoints,
    String redisEndpoint,
    String redisKeyPrefix,
    String logDirectory) implements SampleLogSettings {

    public ApiSettings {
        require(apiBindUrl, "apiBindUrl");
        require(apiChannelEndpoint, "apiChannelEndpoint");
        if (playEndpoints == null || playEndpoints.isEmpty()) {
            throw new IllegalArgumentException("sample.playEndpoints is required");
        }
        require(routeEndpoint, "routeEndpoint");
        if (spotEndpoints == null || spotEndpoints.isEmpty()) {
            throw new IllegalArgumentException("sample.spotEndpoints is required");
        }
        require(redisEndpoint, "redisEndpoint");
        require(redisKeyPrefix, "redisKeyPrefix");
        require(logDirectory, "logDirectory");
    }

    public int apiHttpPort() {
        return java.net.URI.create(apiBindUrl).getPort();
    }

    private static void require(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("sample." + name + " is required");
        }
    }
}
