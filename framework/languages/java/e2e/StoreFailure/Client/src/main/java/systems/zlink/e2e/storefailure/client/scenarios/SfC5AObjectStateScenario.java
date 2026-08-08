package systems.zlink.e2e.storefailure.client.scenarios;

import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

// SF-C5A: ID lookup and page results preserve Missing/Creating/Ready/Unavailable state.
public final class SfC5AObjectStateScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.runObjectStateScenario("SF-C5A");
        return new DiscoveryApiResult(java.util.Set.copyOf(context.options().expectedRids()));
    }
}
