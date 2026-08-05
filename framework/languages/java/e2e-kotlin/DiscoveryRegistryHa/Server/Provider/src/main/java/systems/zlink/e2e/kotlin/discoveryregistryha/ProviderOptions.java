package systems.zlink.e2e.kotlin.discoveryregistryha;

public record ProviderOptions(
    String providerRid,
    String httpEndpoint,
    String apiEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    long redisCommandTimeoutMillis,
    long heartbeatMillis,
    long leaseTtlMillis,
    long pollingMillis,
    long storeFailureGraceMillis,
    String logDir) {
    public static ProviderOptions parse(String[] args) {
        CliOptions options = CliOptions.parse(args);
        return new ProviderOptions(
            options.get("--provider-rid", "api-a"),
            options.get("--http-endpoint"),
            options.get("--api-endpoint"),
            options.get("--redis-location-endpoint"),
            options.get("--location-key-prefix"),
            Long.parseLong(options.get("--redis-command-timeout-ms", "5000")),
            Long.parseLong(options.get("--location-heartbeat-ms", "1000")),
            Long.parseLong(options.get("--location-lease-ttl-ms", "3000")),
            Long.parseLong(options.get("--location-polling-ms", "500")),
            Long.parseLong(options.get("--location-store-failure-grace-ms", "6000")),
            options.get("--log-dir", "logs"));
    }
}
