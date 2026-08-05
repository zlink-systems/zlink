package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;

public final class RlA4DrainAndGreenEndpointScenario {
    private RlA4DrainAndGreenEndpointScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.post(context.adminB() + "/admin/drain");
        context.waitForWeight(context.adminB(), 0);
        context.collectStableProvidersWithout("a4-before-green", "api-b", "api-a");
        context.signal("a4-drained");
        context.waitForSignal("a4-original-down");
        context.collectStableProvidersWithout("a4-old-down", "api-b", "api-a");
        context.waitForSignal("a4-green-up");
        context.waitForTopologyEndpoint(
            "api-b", context.options().apiBGreenEndpoint());
        context.driveUntilEvidence(
            context.adminBGreen(), "a4-green", "RL-A4 green provider did not receive traffic");
        context.signal("a4-green-observed");
        context.signal("a4-restore-ready");
        context.waitForSignal("a4-restored");
        context.waitForTopologyEndpoint("api-b", context.options().apiBEndpoint());
        context.driveUntilEvidence(
            context.adminB(), "a4-restored", "RL-A4 restored provider did not receive traffic");
        System.out.println("scenario RL-A4 passed");
    }
}
