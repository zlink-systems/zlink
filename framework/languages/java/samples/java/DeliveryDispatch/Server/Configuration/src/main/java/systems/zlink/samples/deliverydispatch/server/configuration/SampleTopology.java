package systems.zlink.samples.deliverydispatch.server.configuration;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("sample")
public record SampleTopology(
    String trackingChannelEndpoint,
    String trackingSpotEndpoint,
    String trackingSpotPubEndpoint,
    String customerStreamEndpoint,
    String courierStreamEndpoint,
    String dispatchHttpEndpoint,
    String dispatchSpotEndpoint,
    String dispatchChannelEndpoint,
    String customerSpotEndpoint,
    String customerSpotRouterEndpoint,
    String courierActorNode1SpotEndpoint,
    String courierActorNode2SpotEndpoint,
    String courierSessionSpotEndpoint,
    String redisEndpoint,
    String redisKeyPrefix,
    String courierNode,
    String logDirectory) {

    public SampleTopology {
        trackingChannelEndpoint = value(trackingChannelEndpoint, "tcp://127.0.0.1:48103");
        trackingSpotEndpoint = value(trackingSpotEndpoint, "tcp://127.0.0.1:48118");
        trackingSpotPubEndpoint = value(trackingSpotPubEndpoint, "tcp://127.0.0.1:48120");
        customerStreamEndpoint = value(customerStreamEndpoint, "tcp://127.0.0.1:48104");
        courierStreamEndpoint = value(courierStreamEndpoint, "tcp://127.0.0.1:48105");
        dispatchHttpEndpoint = value(dispatchHttpEndpoint, "http://127.0.0.1:48107");
        dispatchSpotEndpoint = value(dispatchSpotEndpoint, "tcp://127.0.0.1:48108");
        dispatchChannelEndpoint = value(dispatchChannelEndpoint, "tcp://127.0.0.1:48121");
        customerSpotEndpoint = value(customerSpotEndpoint, "tcp://127.0.0.1:48109");
        customerSpotRouterEndpoint = value(customerSpotRouterEndpoint, "tcp://127.0.0.1:48110");
        courierActorNode1SpotEndpoint = value(courierActorNode1SpotEndpoint, "tcp://127.0.0.1:48113");
        courierActorNode2SpotEndpoint = value(courierActorNode2SpotEndpoint, "tcp://127.0.0.1:48114");
        courierSessionSpotEndpoint = value(courierSessionSpotEndpoint, "tcp://127.0.0.1:48119");
        redisEndpoint = required(redisEndpoint, "redisEndpoint");
        redisKeyPrefix = value(redisKeyPrefix, "deliverydispatch:java:");
        courierNode = value(courierNode, "node1");
        logDirectory = required(logDirectory, "logDirectory");
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
