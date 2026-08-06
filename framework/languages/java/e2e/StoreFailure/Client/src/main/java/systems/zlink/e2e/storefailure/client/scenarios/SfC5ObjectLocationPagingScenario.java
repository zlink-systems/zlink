package systems.zlink.e2e.storefailure.client.scenarios;

import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfC5ObjectLocationPagingScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForLivePeerRows();
        context.waitForHealthyStatus();
        context.createObjectLocationFixture("SF-C5", 1_001);
        context.assertObjectLocationPaging("SF-C5", 1_001);
        return new DiscoveryApiResult(java.util.Set.copyOf(context.options().expectedRids()));
    }
}
