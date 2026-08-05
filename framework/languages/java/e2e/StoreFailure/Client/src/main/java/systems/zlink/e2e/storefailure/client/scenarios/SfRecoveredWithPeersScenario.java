package systems.zlink.e2e.storefailure.client.scenarios;

import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfRecoveredWithPeersScenario implements ClientScenario {
    private final String scenarioName;

    public SfRecoveredWithPeersScenario(String scenarioName) {
        this.scenarioName = scenarioName;
    }

    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForRecoveredStatus(scenarioName);
        context.waitForLivePeerRows();
        return context.requestUntilAnyProvider();
    }
}
