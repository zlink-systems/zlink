package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.util.Set;

public final class RlB4RuntimeDrainScenario {
    private RlB4RuntimeDrainScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        Set<String> warm = context.collectProviders("b4-warm", 80, 2);
        ResilienceScenarioContext.ensure(warm.contains("api-a") && warm.contains("api-b"),
            "RL-B4 warmup did not reach both providers: " + warm);
        context.post(context.adminA() + "/admin/drain");
        context.waitForWeight(context.adminA(), 0);
        Set<String> drained = context.collectProvidersExactly("b4-drained", 40);
        ResilienceScenarioContext.ensure(drained.equals(Set.of("api-b")),
            "RL-B4 drained traffic reached an unexpected provider: " + drained);
        ResilienceScenarioContext.ensure(
            !context.hasEvidenceWithPrefix(context.adminA(), "WorkReq", "b4-drained-"),
            "RL-B4 drained provider recorded a new request");
        ResilienceScenarioContext.ensure(context.get(context.adminA() + "/health").contains("ok"),
            "RL-B4 drained provider health failed");
        context.waitForTopology(2);
        context.post(context.adminA() + "/admin/restore");
        context.waitForWeight(context.adminA(), 100);
        Set<String> restored = context.collectProviders("b4-restored", 120, 2);
        ResilienceScenarioContext.ensure(restored.contains("api-a"),
            "RL-B4 restored provider did not receive traffic");
        System.out.println("scenario RL-B4 passed");
    }
}
