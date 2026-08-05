package systems.zlink.e2e.storefailure.provider;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ProviderOptions(
    String rid,
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
    String logDir) {
    public ProviderOptions {
        required(rid, "rid");
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
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }

    private static void positive(long value, String name) {
        if (value <= 0) throw new IllegalArgumentException("e2e." + name + " must be positive");
    }
}
