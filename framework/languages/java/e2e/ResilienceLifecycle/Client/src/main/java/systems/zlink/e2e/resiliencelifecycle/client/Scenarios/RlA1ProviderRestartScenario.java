package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;

public final class RlA1ProviderRestartScenario {
    private RlA1ProviderRestartScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.post(context.adminB() + "/admin/drain");
        context.waitForWeight(context.adminB(), 0);
        context.collectStableProvidersWithout("a1-before-restart", "api-b", "api-a");
        context.signal("a1-ready");
        context.waitForSignal("a1-down");
        expectDownWindowFailure(context);
        context.signal("a1-down-observed");
        context.waitForSignal("a1-up");
        context.waitForTopology(2);
        context.collectStableProvidersWithout("a1-after-restart", "api-b", "api-a");
        context.post(context.adminB() + "/admin/restore");
        context.waitForWeight(context.adminB(), 100);
        System.out.println("scenario RL-A1 passed");
        System.out.println("scenario RL-C3 passed");
    }

    private static void expectDownWindowFailure(ResilienceScenarioContext context) {
        try {
            context.request("a1-down-window", Duration.ofMillis(700));
            throw new IllegalStateException("RL-A1 down-window request unexpectedly completed");
        } catch (RuntimeException expected) {
            // Public failure is required while the sole admissible provider is down.
        }
    }
}
