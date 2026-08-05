package systems.zlink.e2e.storefailure.client.scenarios;

import com.fasterxml.jackson.databind.JsonNode;
import java.time.Duration;
import java.util.Set;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;
import systems.zlink.e2e.storefailure.client.support.ScenarioAssert;

public final class SfD3StatusTransitionScenario implements ClientScenario {
    private final Stage stage;

    public SfD3StatusTransitionScenario(Stage stage) {
        this.stage = stage;
    }

    @Override
    public DiscoveryApiResult run(ClientContext context) {
        return switch (stage) {
            case HEALTHY -> healthy(context);
            case OUTAGE -> outage(context);
            case RECOVERED -> recovered(context);
        };
    }

    private static DiscoveryApiResult healthy(ClientContext context) {
        JsonNode status = context.waitForHealthyStatus();
        ScenarioAssert.that(!status.path("lastRefreshAt").asText("").isBlank(),
            "SF-D3 healthy status did not expose last refresh time");
        ScenarioAssert.that(!status.path("ownerLeaseRenewedAt").asText("").isBlank(),
            "SF-D3 healthy status did not expose owner lease renewal time");
        return context.requestUntilAnyProvider();
    }

    private static DiscoveryApiResult outage(ClientContext context) {
        JsonNode status = context.waitForStatus(
            Duration.ofMillis(context.options().locationHeartbeatMillis() * 8),
            current -> !current.path("storeHealthy").asBoolean(true)
                && !current.path("ownerLeaseHealthy").asBoolean(true)
                && !current.path("lastError").asText("").isBlank(),
            "SF-D3 outage did not surface as unhealthy status with owner lease failure");
        ScenarioAssert.that(!status.path("watchEnabled").asBoolean(true),
            "SF-D3 outage status did not expose polling-mode watch state");
        ScenarioAssert.that(status.path("pollingIntervalMillis").asLong(0) > 0,
            "SF-D3 outage status did not expose polling interval");
        return new DiscoveryApiResult(Set.of());
    }

    private static DiscoveryApiResult recovered(ClientContext context) {
        JsonNode status = context.waitForRecoveredStatus("SF-D3");
        ScenarioAssert.that(!status.path("lastRefreshAt").asText("").isBlank(),
            "SF-D3 recovered status did not expose last refresh time");
        ScenarioAssert.that(!status.path("ownerLeaseRenewedAt").asText("").isBlank(),
            "SF-D3 recovered status did not expose owner lease renewal time");
        return context.requestUntilAnyProvider();
    }

    public enum Stage {
        HEALTHY,
        OUTAGE,
        RECOVERED
    }
}
