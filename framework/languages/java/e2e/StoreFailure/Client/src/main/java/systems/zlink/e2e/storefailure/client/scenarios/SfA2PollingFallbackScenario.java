package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfA2PollingFallbackScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForPollingOnlyStatus();
        context.waitForLivePeerRows();
        for (String absentRid : context.options().expectedAbsentRids()) {
            context.waitForPeerRowsExcluding(absentRid,
                Duration.ofMillis(context.options().locationPollingMillis() * 8
                    + context.options().locationLeaseTtlMillis()));
        }
        return context.requestUntilAnyProvider();
    }
}
