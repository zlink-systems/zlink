package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;
import systems.zlink.e2e.storefailure.shared.Wait;

public final class SfC1CrashLeaseExpiryScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForPeerRowsExcluding(
            context.options().deadRid(),
            Duration.ofMillis(context.options().locationLeaseTtlMillis() * 2
                + context.options().locationPollingMillis() * 4));
        Wait.sleep(Duration.ofMillis(context.options().locationPollingMillis() * 4));
        return context.requestSurvivorOnly();
    }
}
