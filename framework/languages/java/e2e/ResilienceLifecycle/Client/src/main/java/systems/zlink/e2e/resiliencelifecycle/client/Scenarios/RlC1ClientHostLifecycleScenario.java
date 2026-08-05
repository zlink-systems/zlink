package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlC1ClientHostLifecycleScenario {
    private RlC1ClientHostLifecycleScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        for (int index = 0; index < 12; index++) {
            Contracts.WorkRes reply = context.request("rl-c1-" + index, Duration.ofSeconds(3));
            ResilienceScenarioContext.ensure(reply.value().equals("work:rl-c1-" + index),
                "RL-C1 request payload mismatch for " + index);
        }
        Contracts.WorkRes followUp =
            context.request("rl-c1-after-cleanup", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure(
            followUp.value().equals("work:rl-c1-after-cleanup"),
            "RL-C1 follow-up payload mismatch");
        System.out.println("scenario RL-C1 passed");
    }
}
