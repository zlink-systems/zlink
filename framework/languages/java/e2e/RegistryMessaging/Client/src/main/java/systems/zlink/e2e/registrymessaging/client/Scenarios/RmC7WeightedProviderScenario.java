package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC7WeightedProviderScenario {
    private RmC7WeightedProviderScenario() {
    }

    public static void run(ZLinkHttpClient directConsumer) {
        RmC3MultiProviderDistributionScenario.run(directConsumer, "RM-C7", "weighted-", 300, true);
    }
}
