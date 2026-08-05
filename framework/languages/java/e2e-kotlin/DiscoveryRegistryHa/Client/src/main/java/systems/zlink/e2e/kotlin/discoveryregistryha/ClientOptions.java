package systems.zlink.e2e.kotlin.discoveryregistryha;

import java.util.List;

public record ClientOptions(
    String scenario,
    String consumerHttpEndpoint,
    List<String> expectedRids,
    String deadRid,
    List<String> expectedAbsentRids,
    long locationHeartbeatMillis,
    long locationLeaseTtlMillis,
    long locationPollingMillis,
    long locationStoreFailureGraceMillis) {
    public static ClientOptions parse(String[] args) {
        CliOptions options = CliOptions.parse(args);
        return new ClientOptions(
            options.get("--scenario"),
            options.get("--consumer-http-endpoint"),
            options.csv("--expected-rids"),
            options.get("--dead-rid", "api-b"),
            options.csv("--expected-absent-rids"),
            Long.parseLong(options.get("--location-heartbeat-ms", "1000")),
            Long.parseLong(options.get("--location-lease-ttl-ms", "3000")),
            Long.parseLong(options.get("--location-polling-ms", "500")),
            Long.parseLong(options.get("--location-store-failure-grace-ms", "6000")));
    }
}
