package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlB1CancellationCleanupScenario {
    private RlB1CancellationCleanupScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        try {
            context.request("timeout", Duration.ofMillis(300));
            throw new IllegalStateException("RL-B1 timeout request unexpectedly completed");
        } catch (RuntimeException expected) {
            context.waitForEvidenceAny("TimeoutStarted", context.adminA(), context.adminB());
        }
        ResilienceScenarioContext.sleep(1800);
        Contracts.WorkRes followUp = context.request("b1-follow-up", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:b1-follow-up".equals(followUp.value()),
            "RL-B1 follow-up payload mismatch");
        System.out.println("scenario RL-B1 passed");
    }
}
