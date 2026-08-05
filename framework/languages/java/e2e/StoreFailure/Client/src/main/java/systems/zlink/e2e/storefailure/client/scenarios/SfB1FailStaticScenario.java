package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfB1FailStaticScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        DiscoveryApiResult result = context.driveRequests(
            "sf-b1-outage",
            Duration.ofMillis(Math.max(1,
                (long) (context.options().locationLeaseTtlMillis() * 0.7))),
            "SF-B1");
        context.waitForUnhealthyStatus("SF-B1");
        context.waitForOwnerLeaseFailure("SF-B1");
        return result;
    }
}
