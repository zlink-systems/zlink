package systems.zlink.e2e.storefailure.client.scenarios;

import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfB3OwnerLeaseScenario implements ClientScenario {
    private final boolean expired;

    public SfB3OwnerLeaseScenario(boolean expired) {
        this.expired = expired;
    }

    @Override
    public DiscoveryApiResult run(ClientContext context) {
        if (expired) {
            return context.requestInstanceUnavailable("SF-B3", "sf-b3-owner");
        }
        return context.requestInstanceOwner(
            "SF-B3",
            "sf-b3-owner",
            "sf-b3-before-lease",
            1);
    }
}
