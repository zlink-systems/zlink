package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfD1ShortOutageRecoveryScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        return context.driveRequests(
            "sf-d1",
            Duration.ofMillis(context.options().locationLeaseTtlMillis() * 2),
            "SF-D1");
    }
}
