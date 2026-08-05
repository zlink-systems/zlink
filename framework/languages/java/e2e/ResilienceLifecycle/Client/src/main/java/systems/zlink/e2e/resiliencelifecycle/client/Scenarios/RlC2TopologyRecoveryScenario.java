package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;

public final class RlC2TopologyRecoveryScenario {
    private RlC2TopologyRecoveryScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.signal("c2-ready");
        context.waitForSignal("c2-crashed");
        context.waitForTopologyWithout("api-b", 30);
        context.collectStableProvidersWithoutFailures("c2-after-crash", "api-b", "api-a");
        context.signal("c2-survivor-observed");
        context.waitForSignal("c2-restored");
        context.waitForTopologyEndpoint("api-b", context.options().apiBEndpoint());
        context.driveUntilEvidence(
            context.adminB(), "c2-restored", "RL-C2 restored provider traffic missing");
        System.out.println("scenario RL-C2 passed");
    }
}
