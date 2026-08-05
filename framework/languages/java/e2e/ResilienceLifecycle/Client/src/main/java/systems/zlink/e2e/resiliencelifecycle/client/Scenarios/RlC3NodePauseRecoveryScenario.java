package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;

public final class RlC3NodePauseRecoveryScenario {
    private RlC3NodePauseRecoveryScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        RlA1ProviderRestartScenario.run(context);
    }
}
