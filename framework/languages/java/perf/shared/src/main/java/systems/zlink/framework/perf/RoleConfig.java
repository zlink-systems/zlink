package systems.zlink.framework.perf;

import java.util.List;
import java.util.Map;

// The role config file the common runner writes before a role starts (§5.1); the role executable reads only this.
// Field names are the runner's (framework/perf/runner/roles.py); an unknown property is a schema mismatch.
public record RoleConfig(
        String runId,
        String cellId,
        String configHash,
        String language,
        String role,
        int roleInstance,
        String scenario,
        String mode,
        String terminal,
        String topology,
        String channelName,
        String meshName,
        Map<String, String> transportEndpoints,
        String peerEndpoint,
        String metricsUrl,
        String applicationTriggerUrl,
        boolean source,
        String objectRole,
        boolean awaitRemoteTargets,
        StoreConfig store,
        List<String> spotIds,
        List<String> actorIds,
        Integer spotCount,
        Integer subscriberCount,
        WorkerConfig worker,
        String executionMode,
        Workload workload,
        DiagnosticsConfig diagnostics,
        Map<String, Object> provenance) {

    public RoleConfig {
        if (language == null || language.isBlank()) {
            throw new IllegalArgumentException("The runner role config must include language.");
        }
    }

    public record Workload(
            int payloadSize,
            double durationSeconds,
            double warmupSeconds,
            int inflight,
            Integer connections,
            Integer logicalStreams,
            int clientCount,
            Integer connectConcurrency,
            int requestTimeoutMs,
            int correlationExpiryMs,
            int driverTimeoutMs,
            int setupTimeoutMs,
            int adminTimeoutMs,
            int socketSendTimeoutMs) {}

    // §20: the run-owned Redis and this cell's namespace; null when the scenario needs no Store.
    public record StoreConfig(String provider, String endpoint, String containerId, String image, String imageDigest,
            String namespace) {}

    // §5.2: the public worker options and the CPU task every callback runs (§10.8).
    public record WorkerConfig(String algorithm, int taskMillis, int minThreads, int maxThreads,
            int idleTimeoutMs, int workerTimeoutMs) {}

    public record DiagnosticsConfig(String level, String flowFile) {}

    /** The role's first listener; roles with several transports read {@link #transportEndpoints} by key. */
    public String listenerEndpoint() {
        return transportEndpoints.isEmpty() ? null : transportEndpoints.values().iterator().next();
    }
}
