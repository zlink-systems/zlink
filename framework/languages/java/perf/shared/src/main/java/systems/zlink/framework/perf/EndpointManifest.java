package systems.zlink.framework.perf;

import java.util.List;
import java.util.Map;

// The client's endpoint manifest (§5.1, framework/perf/runner/runner.py `manifest`).
public record EndpointManifest(
        String runId,
        String cellId,
        String configHash,
        String language,
        RoleConfig.Workload workload,
        List<EndpointRole> roles,
        Map<String, Object> provenance) {

    public record EndpointRole(
            String role,
            int roleInstance,
            String configFile,
            String streamEndpoint,
            String applicationTriggerUrl,
            MetricsEndpoint metrics,
            Map<String, String> transportEndpoints,
            List<String> spotIds,
            List<String> actorIds) {}

    public record MetricsEndpoint(String transport, String baseUrl) {}
}
