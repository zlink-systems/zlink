package systems.zlink.e2e.channelegress.role;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record RoleOptions(
    String role,
    String rid,
    String instanceMarker,
    String httpEndpoint,
    String redisLocationEndpoint,
    String locationKeyPrefix,
    String logDirectory,
    String gameEndpoint,
    String gamePeerRids,
    String gamePeerEndpoints,
    String gameServers,
    String gameClients,
    String auditEndpoint,
    String auditPeerRids,
    String auditPeerEndpoints,
    String auditServers,
    String auditClients,
    String workflowEndpoint,
    int workflowPort,
    int workflowWeight,
    boolean workflowClient,
    boolean workflowServer,
    boolean instanceSpot,
    boolean objectClient) {
    public RoleOptions {
        required(role, "role");
        required(rid, "rid");
        required(instanceMarker, "instance-marker");
        required(httpEndpoint, "http-endpoint");
        required(redisLocationEndpoint, "redis-location-endpoint");
        required(locationKeyPrefix, "location-key-prefix");
        required(logDirectory, "log-directory");
        gamePeerRids = empty(gamePeerRids);
        gamePeerEndpoints = empty(gamePeerEndpoints);
        gameServers = empty(gameServers);
        gameClients = empty(gameClients);
        auditPeerRids = empty(auditPeerRids);
        auditPeerEndpoints = empty(auditPeerEndpoints);
        auditServers = empty(auditServers);
        auditClients = empty(auditClients);
        workflowEndpoint = empty(workflowEndpoint);
        if (workflowWeight < 0) {
            throw new IllegalArgumentException("e2e.workflow-weight must not be negative");
        }
    }

    public String[] gameServerNames() {
        return values(gameServers);
    }

    public String[] gameClientNames() {
        return values(gameClients);
    }

    public String[] auditServerNames() {
        return values(auditServers);
    }

    public String[] auditClientNames() {
        return values(auditClients);
    }

    public String[] gamePeerRidValues() {
        return values(gamePeerRids);
    }

    public String[] gamePeerEndpointValues() {
        return values(gamePeerEndpoints);
    }

    public String[] auditPeerRidValues() {
        return values(auditPeerRids);
    }

    public String[] auditPeerEndpointValues() {
        return values(auditPeerEndpoints);
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("e2e." + name + " is required");
        }
    }

    private static String empty(String value) {
        return value == null ? "" : value;
    }

    private static String[] values(String value) {
        return value.isBlank() ? new String[0] : value.split(",", -1);
    }
}
