package systems.zlink.samples.tictactoe.server.configuration;

import java.util.List;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record PlaySettings(
    String apiChannelEndpoint,
    String playEndpoint,
    List<String> playEndpoints,
    String spotEndpoint,
    String spotPubSubEndpoint,
    String redisEndpoint,
    String redisKeyPrefix,
    String peerSpotEndpoint,
    String peerSpotPubSubEndpoint,
    String logDirectory) implements SampleLogSettings {

    public PlaySettings {
        require(apiChannelEndpoint, "apiChannelEndpoint");
        require(playEndpoint, "playEndpoint");
        if (playEndpoints == null || playEndpoints.isEmpty()) {
            throw new IllegalArgumentException("sample.playEndpoints is required");
        }
        require(spotEndpoint, "spotEndpoint");
        require(spotPubSubEndpoint, "spotPubSubEndpoint");
        require(redisEndpoint, "redisEndpoint");
        require(redisKeyPrefix, "redisKeyPrefix");
        require(peerSpotEndpoint, "peerSpotEndpoint");
        require(peerSpotPubSubEndpoint, "peerSpotPubSubEndpoint");
        require(logDirectory, "logDirectory");
    }

    public int playIndex() {
        int index = playEndpoints.indexOf(playEndpoint);
        return index >= 0 ? index : 0;
    }

    public String routeEndpoint() {
        return spotEndpoint;
    }

    private static void require(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("sample." + name + " is required");
        }
    }
}
