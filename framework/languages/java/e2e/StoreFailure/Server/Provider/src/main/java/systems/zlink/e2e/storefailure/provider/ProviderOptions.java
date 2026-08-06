package systems.zlink.e2e.storefailure.provider;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ProviderOptions(
    String rid,
    String lifecycleId,
    String httpEndpoint,
    String channelEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    long redisCommandTimeoutMillis,
    long heartbeatMillis,
    long leaseTtlMillis,
    long pollingMillis,
    long storeFailureGraceMillis,
    String evidenceFile,
    String logDir,
    boolean c4Roles,
    String c4RouteAEndpoint,
    String c4RouteBEndpoint,
    int c4ClientServerPort,
    int c4FanoutPort) {
    public ProviderOptions {
        required(rid, "rid");
        required(lifecycleId, "lifecycle-id");
        required(channelEndpoint, "channel-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        positive(redisCommandTimeoutMillis, "redis-command-timeout-millis");
        positive(heartbeatMillis, "heartbeat-millis");
        positive(leaseTtlMillis, "lease-ttl-millis");
        positive(pollingMillis, "polling-millis");
        positive(storeFailureGraceMillis, "store-failure-grace-millis");
        required(logDir, "log-dir");
        evidenceFile = evidenceFile == null ? "" : evidenceFile;
        httpEndpoint = httpEndpoint == null ? "" : httpEndpoint;
        c4RouteAEndpoint = c4RouteAEndpoint == null ? "" : c4RouteAEndpoint;
        c4RouteBEndpoint = c4RouteBEndpoint == null ? "" : c4RouteBEndpoint;
        if (c4Roles) {
            required(c4RouteAEndpoint, "c4-route-a-endpoint");
            required(c4RouteBEndpoint, "c4-route-b-endpoint");
            positive(c4ClientServerPort, "c4-client-server-port");
            positive(c4FanoutPort, "c4-fanout-port");
        }
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }

    private static void positive(long value, String name) {
        if (value <= 0) throw new IllegalArgumentException("e2e." + name + " must be positive");
    }
}
