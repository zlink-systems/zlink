package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfB2GraceExceededScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        DiscoveryApiResult result = context.driveRequests(
            "sf-b2-grace",
            Duration.ofMillis(context.options().locationStoreFailureGraceMillis()
                + context.options().locationHeartbeatMillis() * 2),
            "SF-B2");
        context.waitForUnhealthyStatus("SF-B2");
        return result;
    }
}
